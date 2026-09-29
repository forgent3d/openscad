/*
 * Forgent3D structured export (`-o out.fgjson`): the evaluated node tree as JSON.
 *
 * The .csg export is the same tree printed as OpenSCAD text, and a consumer that rebuilds geometry from
 * it runs into everything text loses:
 *  - numbers are cut to 6 significant digits (here: shortest text that reads back to the same double);
 *  - no node says where it came from (here: every node carries file:line:col of the call that made it,
 *    and user-module groups carry the module's name — the instance path is the chain of ancestors);
 *  - strings are printed unescaped, inf/nan are bare words, some arguments are positional (here: JSON);
 *  - the console (warnings, echo, assert) is a separate stream without structure (here: `log`, each
 *    message with its location — echo included, which the console prints without one);
 *  - the Customizer parameters need a second run with `-o out.param` (here: `params`, same evaluation).
 *
 * Node arguments keep the .csg's names and value conventions (`size`, `center`, `$fn`…), so a reader of
 * one reads the other; only the text layer differs. `-O fgjson/csg=true` also embeds the .csg text of
 * this very evaluation (full precision too), for an oracle that must render exactly the tree the
 * consumer built — a second run could differ (rands() without a seed).
 *
 * Shape:
 *   { "format": "forgent3d-openscad-tree", "version": 1,
 *     "tree": [node…], "params": {…}, "log": [{"group", "text", "at"?}…], "csg"?: "…",
 *     "files": ["/project/model.scad", …] }
 *   node = { "node": "cube", "modifier"?: "%"|"#", "module"?: "name" | "call"?: "translate",
 *            "at"?: [file index, first line, first column, last line, last column],
 *            "args": {…}, "children"?: [node…] }
 * Non-finite numbers are the strings "inf", "-inf", "nan" (JSON has no such numbers). ListNode and the
 * root are not nodes: their children are spliced into the parent's list, as the .csg does.
 */

#include "io/export.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/AST.h"
#include "core/CgalAdvNode.h"
#include "core/ColorNode.h"
#include "core/CsgOpNode.h"
#include "core/ImportNode.h"
#include "core/LinearExtrudeNode.h"
#include "core/ModuleInstantiation.h"
#include "core/OffsetNode.h"
#include "core/ProjectionNode.h"
#include "core/RenderNode.h"
#include "core/RoofNode.h"
#include "core/RotateExtrudeNode.h"
#include "core/SourceFile.h"
#include "core/SurfaceNode.h"
#include "core/TextNode.h"
#include "core/TransformNode.h"
#include "core/Tree.h"
#include "core/node.h"
#include "core/primitives.h"
#include "utils/full_precision.h"
#include "utils/printutils.h"
#include "geometry/Geometry.h"
#include "geometry/GeometryEvaluator.h"
#include "geometry/Polygon2d.h"
#include "geometry/PolySet.h"
#include "geometry/PolySetUtils.h"

namespace {

/** Past this many messages the log says how many it dropped: a loop that echoes a million lines must not fill memory. */
constexpr size_t MAX_MESSAGES = 10000;

struct Logged {
  Message message;
  Location origin;
};
std::vector<Logged> logged;
size_t dropped = 0;

void tap(const Message& message)
{
  if (logged.size() >= MAX_MESSAGES) {
    ++dropped;
    return;
  }
  logged.push_back({message, message_origin ? *message_origin : Location::NONE});
}

std::string quote(const std::string& text)
{
  std::string out;
  out.reserve(text.size() + 2);
  out += '"';
  for (unsigned char c : text) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (c < 0x20) {
        static const char *hex = "0123456789abcdef";
        out += "\\u00";
        out += hex[c >> 4];
        out += hex[c & 15];
      } else {
        out += static_cast<char>(c);
      }
    }
  }
  out += '"';
  return out;
}

std::string num(double value)
{
  return std::isfinite(value) ? shortest_double(value) : quote(shortest_double(value));
}

std::string numf(float value)
{
  if (!std::isfinite(value)) return quote(shortest_double(value));
  char buf[32];
  const auto result = std::to_chars(buf, buf + sizeof(buf), value);
  return {buf, result.ptr};
}

std::string boolean(bool value) { return value ? "true" : "false"; }

std::string vec(std::initializer_list<double> values)
{
  std::string out = "[";
  bool first = true;
  for (double v : values) {
    if (!first) out += ",";
    first = false;
    out += num(v);
  }
  return out + "]";
}

/**
 * text()'s outline as JSON: `[[start, segment…], …]`, one array per closed contour over all glyphs (OpenSCAD fills
 * them together, nonzero winding, orientation as the font has it). A segment is `[x, y]` (a line), `[cx, cy, x, y]`
 * (a quadratic Bézier) or `[c1x, c1y, c2x, c2y, x, y]` (a cubic) — each from where the previous one ended.
 */
class ContourSink : public FreetypeRenderer::OutlineSink
{
public:
  void move_to(const Vector2d& to) override
  {
    close();
    contour = "[" + vec({to[0], to[1]});
  }
  void line_to(const Vector2d& to) override { add({to[0], to[1]}); }
  void conic_to(const Vector2d& c, const Vector2d& to) override { add({c[0], c[1], to[0], to[1]}); }
  void cubic_to(const Vector2d& c1, const Vector2d& c2, const Vector2d& to) override { add({c1[0], c1[1], c2[0], c2[1], to[0], to[1]}); }
  std::string finish()
  {
    close();
    return out + "]";
  }

private:
  std::string out = "[";
  std::string contour;
  size_t segments = 0;
  void add(std::initializer_list<double> values)
  {
    contour += "," + vec(values);
    ++segments;
  }
  void close()
  {
    if (segments > 0) {
      if (out.size() > 1) out += ",";
      out += contour + "]";
    }
    contour.clear();
    segments = 0;
  }
};

}  // namespace

/** Not in the anonymous namespace: FreetypeRenderer::Params befriends it by name to read text()'s parameters. */
class FgjsonWriter
{
public:
  explicit FgjsonWriter(std::ostream& out, const Tree *tree = nullptr, bool mesh = false) : out(out), tree(tree), mesh(mesh) {}

  /** A node list: `nodes` spliced through ListNode/RootNode, each written as one JSON object. */
  void list(const AbstractNode& node, bool background, bool highlight, bool& first)
  {
    background = background || node.modinst->isBackground();
    highlight = highlight || node.modinst->isHighlight();
    if (dynamic_cast<const ListNode *>(&node) || dynamic_cast<const RootNode *>(&node)) {
      for (const auto& child : node.getChildren()) list(*child, background, highlight, first);
      return;
    }
    if (!first) out << ",";
    first = false;
    write(node, background, highlight);
  }

  std::string at(const Location& loc)
  {
    if (loc.isNone()) return "";
    const auto file = loc.fileName();
    auto found = fileIndex.find(file);
    size_t index;
    if (found == fileIndex.end()) {
      index = files.size();
      fileIndex.emplace(file, index);
      files.push_back(file);
    } else {
      index = found->second;
    }
    std::ostringstream s;
    s << "[" << index << "," << loc.firstLine() << "," << loc.firstColumn() << "," << loc.lastLine() << "," << loc.lastColumn() << "]";
    return s.str();
  }

  std::vector<std::string> files;

private:
  std::ostream& out;
  const Tree *tree;
  /** -O fgjson/mesh=true: inline OpenSCAD's render of the nodes a consumer has no exact counterpart for. */
  bool mesh;
  std::unordered_map<std::string, size_t> fileIndex;
  std::vector<std::pair<const char *, std::string>> args;

  void arg(const char *name, std::string value) { args.emplace_back(name, std::move(value)); }

  void write(const AbstractNode& node, bool background, bool highlight)
  {
    // the .csg's keyword: TransformNode::name() is "transform" but it prints itself as multmatrix(…)
    out << "{\"node\":" << quote(dynamic_cast<const TransformNode *>(&node) ? std::string("multmatrix") : node.name());
    if (background) out << ",\"modifier\":\"%\"";
    else if (highlight) out << ",\"modifier\":\"#\"";
    const auto verbose = node.verbose_name();
    if (verbose.rfind("module ", 0) == 0) {
      out << ",\"module\":" << quote(verbose.substr(7));
    } else if (node.modinst->name() != node.name() && !(dynamic_cast<const TransformNode *>(&node) && node.modinst->name() == "multmatrix")) {
      out << ",\"call\":" << quote(node.modinst->name());
    }
    const auto where = at(node.modinst->location());
    if (!where.empty()) out << ",\"at\":" << where;

    args.clear();
    collectArgs(node);
    out << ",\"args\":{";
    for (size_t i = 0; i < args.size(); ++i) {
      if (i) out << ",";
      out << "\"" << args[i].first << "\":" << args[i].second;
    }
    out << "}";

    if (mesh && tree && wantsRender(node)) rendered(node);

    if (!node.getChildren().empty()) {
      out << ",\"children\":[";
      bool first = true;
      for (const auto& child : node.getChildren()) list(*child, false, false, first);
      out << "]";
    }
    out << "}";
  }

  /**
   * The nodes that only a render can answer: files read as meshes or drawings (import, surface), and the
   * operations whose result is not a simple function of their children's exact shapes (projection without cut,
   * roof, a chamfered offset). The consumer decides whether it uses the render (Write.scad's DXF fonts it builds
   * exactly); the render costs nothing where the file is small, and only this option asks for it.
   */
  static bool wantsRender(const AbstractNode& node)
  {
    const auto name = node.name();
    if (name == "import" || name == "surface" || name == "roof") return true;
    if (const auto *n = dynamic_cast<const ProjectionNode *>(&node)) return !n->cut_mode;
    if (const auto *n = dynamic_cast<const OffsetNode *>(&node)) return n->chamfer;
    return false;
  }

  /** `"rendered": {"dim": 3, "points", "faces"}` / `{"dim": 2, "points", "paths"}` — OpenSCAD's own render of `node`. */
  void rendered(const AbstractNode& node)
  {
#if defined(ENABLE_CGAL) || defined(ENABLE_MANIFOLD)
    GeometryEvaluator evaluator(*tree);
    const auto geom = evaluator.evaluateGeometry(node, false);
    if (!geom || geom->isEmpty()) {
      out << ",\"rendered\":null";
      return;
    }
    if (const auto poly = std::dynamic_pointer_cast<const Polygon2d>(geom)) {
      std::string points = "[", paths = "[";
      size_t base = 0;
      for (const auto& outline : poly->outlines()) {
        if (paths.size() > 1) paths += ",";
        paths += "[";
        for (size_t k = 0; k < outline.vertices.size(); ++k) {
          if (base + k) points += ",";
          points += vec({outline.vertices[k][0], outline.vertices[k][1]});
          if (k) paths += ",";
          paths += std::to_string(base + k);
        }
        paths += "]";
        base += outline.vertices.size();
      }
      out << ",\"rendered\":{\"dim\":2,\"points\":" << points << "],\"paths\":" << paths << "]}";
      return;
    }
    const auto ps = PolySetUtils::getGeometryAsPolySet(geom);
    if (!ps) {
      out << ",\"rendered\":null";
      return;
    }
    out << ",\"rendered\":{\"dim\":3,\"points\":[";
    for (size_t k = 0; k < ps->vertices.size(); ++k) {
      if (k) out << ",";
      out << vec({ps->vertices[k][0], ps->vertices[k][1], ps->vertices[k][2]});
    }
    out << "],\"faces\":[";
    for (size_t k = 0; k < ps->indices.size(); ++k) {
      if (k) out << ",";
      out << "[";
      for (size_t i = 0; i < ps->indices[k].size(); ++i) out << (i ? "," : "") << ps->indices[k][i];
      out << "]";
    }
    out << "]}";
#else
    LOG(message_group::Warning, node.modinst->location(), "", "This OpenSCAD build has no geometry backend: %1$s() is not rendered", node.name());
    out << ",\"rendered\":null";
#endif
  }

  void segments(double fn, double fa, double fs)
  {
    arg("$fn", num(fn));
    arg("$fa", num(fa));
    arg("$fs", num(fs));
  }

  /** The .csg's arguments for `node` (each node's toString()), as JSON values. */
  void collectArgs(const AbstractNode& node)
  {
    if (const auto *n = dynamic_cast<const CubeNode *>(&node)) {
      arg("size", vec({n->x, n->y, n->z}));
      arg("center", boolean(n->center));
    } else if (const auto *n = dynamic_cast<const SphereNode *>(&node)) {
      segments(n->fn, n->fa, n->fs);
      arg("r", num(n->r));
    } else if (const auto *n = dynamic_cast<const CylinderNode *>(&node)) {
      segments(n->fn, n->fa, n->fs);
      arg("h", num(n->h));
      arg("r1", num(n->r1));
      arg("r2", num(n->r2));
      arg("center", boolean(n->center));
    } else if (const auto *n = dynamic_cast<const SquareNode *>(&node)) {
      arg("size", vec({n->x, n->y}));
      arg("center", boolean(n->center));
    } else if (const auto *n = dynamic_cast<const CircleNode *>(&node)) {
      segments(n->fn, n->fa, n->fs);
      arg("r", num(n->r));
    } else if (const auto *n = dynamic_cast<const PolygonNode *>(&node)) {
      std::string points = "[";
      for (size_t i = 0; i < n->points.size(); ++i) {
        if (i) points += ",";
        points += vec({n->points[i][0], n->points[i][1]});
      }
      arg("points", points + "]");
      if (n->paths.empty()) {
        arg("paths", "null");
      } else {
        std::string paths = "[";
        for (size_t i = 0; i < n->paths.size(); ++i) {
          if (i) paths += ",";
          paths += "[";
          for (size_t k = 0; k < n->paths[i].size(); ++k) {
            if (k) paths += ",";
            paths += std::to_string(n->paths[i][k]);
          }
          paths += "]";
        }
        arg("paths", paths + "]");
      }
      arg("convexity", std::to_string(n->convexity));
    } else if (const auto *n = dynamic_cast<const PolyhedronNode *>(&node)) {
      std::string points = "[";
      for (size_t i = 0; i < n->points.size(); ++i) {
        if (i) points += ",";
        points += vec({n->points[i][0], n->points[i][1], n->points[i][2]});
      }
      arg("points", points + "]");
      std::string faces = "[";
      for (size_t i = 0; i < n->faces.size(); ++i) {
        if (i) faces += ",";
        faces += "[";
        for (size_t k = 0; k < n->faces[i].size(); ++k) {
          if (k) faces += ",";
          faces += std::to_string(n->faces[i][k]);
        }
        faces += "]";
      }
      arg("faces", faces + "]");
      arg("convexity", std::to_string(n->convexity));
    } else if (const auto *n = dynamic_cast<const TransformNode *>(&node)) {
      std::string m = "[";
      for (int j = 0; j < 4; ++j) {
        if (j) m += ",";
        m += vec({n->matrix(j, 0), n->matrix(j, 1), n->matrix(j, 2), n->matrix(j, 3)});
      }
      arg("m", m + "]");
    } else if (const auto *n = dynamic_cast<const ColorNode *>(&node)) {
      arg("c", "[" + numf(n->color.r()) + "," + numf(n->color.g()) + "," + numf(n->color.b()) + "," + numf(n->color.a()) + "]");
    } else if (const auto *n = dynamic_cast<const RenderNode *>(&node)) {
      arg("convexity", std::to_string(n->convexity));
    } else if (const auto *n = dynamic_cast<const CgalAdvNode *>(&node)) {
      if (n->type == CgalAdvType::MINKOWSKI) {
        arg("convexity", std::to_string(n->convexity));
      } else if (n->type == CgalAdvType::RESIZE) {
        arg("newsize", vec({n->newsize[0], n->newsize[1], n->newsize[2]}));
        arg("auto", "[" + boolean(n->autosize[0]) + "," + boolean(n->autosize[1]) + "," + boolean(n->autosize[2]) + "]");
        arg("convexity", std::to_string(n->convexity));
      }
    } else if (const auto *n = dynamic_cast<const LinearExtrudeNode *>(&node)) {
      const double height = n->height.norm();
      arg("height", num(height));
      if (height > 0) {
        const Vector3d v = n->height / height;
        if (v[2] < 1) arg("v", vec({v[0], v[1], v[2]}));
      }
      if (n->center) arg("center", "true");
      if (n->has_twist) arg("twist", num(n->twist));
      if (n->has_slices) arg("slices", std::to_string(n->slices));
      if (n->has_segments) arg("segments", std::to_string(n->segments));
      if (n->scale_x != n->scale_y) arg("scale", vec({n->scale_x, n->scale_y}));
      else if (n->scale_x != 1.0) arg("scale", num(n->scale_x));
      if (!(n->has_slices && n->has_segments)) segments(n->fn, n->fa, n->fs);
      if (n->convexity > 1) arg("convexity", std::to_string(n->convexity));
    } else if (const auto *n = dynamic_cast<const RotateExtrudeNode *>(&node)) {
      arg("angle", num(n->angle));
      arg("start", num(n->start));
      arg("convexity", std::to_string(n->convexity));
      segments(n->fn, n->fa, n->fs);
    } else if (const auto *n = dynamic_cast<const OffsetNode *>(&node)) {
      if (n->join_type == Clipper2Lib::JoinType::Round) {
        arg("r", num(n->delta));
      } else {
        arg("delta", num(n->delta));
        arg("chamfer", boolean(n->chamfer));
      }
      segments(n->fn, n->fa, n->fs);
    } else if (const auto *n = dynamic_cast<const ProjectionNode *>(&node)) {
      arg("cut", boolean(n->cut_mode));
      arg("convexity", std::to_string(n->convexity));
    } else if (const auto *n = dynamic_cast<const TextNode *>(&node)) {
      const auto& p = n->params;
      arg("text", quote(p.text));
      arg("size", num(p.size));
      arg("spacing", num(p.spacing));
      arg("font", quote(p.font));
      arg("direction", quote(p.direction));
      arg("language", quote(p.language));
      if (!p.script.empty()) arg("script", quote(p.script));
      arg("halign", quote(p.halign));
      arg("valign", quote(p.valign));
      segments(p.fn, p.fa, p.fs);
      // the glyphs themselves, exact (FreetypeRenderer::outlines); absent when there is no font to shape with
      ContourSink sink;
      if (FreetypeRenderer().outlines(n->get_params(), sink)) arg("contours", sink.finish());
    } else if (const auto *n = dynamic_cast<const ImportNode *>(&node)) {
      arg("file", quote(static_cast<const std::string&>(n->filename)));
      if (n->id) arg("id", quote(n->id.get()));
      if (n->layer) arg("layer", quote(n->layer.get()));
      arg("origin", vec({n->origin_x, n->origin_y}));
      if (n->type == ImportType::SVG) arg("dpi", num(n->dpi));
      arg("scale", num(n->scale));
      arg("center", boolean(n->center));
      arg("convexity", std::to_string(n->convexity));
      segments(n->fn, n->fa, n->fs);
    } else if (const auto *n = dynamic_cast<const SurfaceNode *>(&node)) {
      arg("file", quote(static_cast<const std::string&>(n->filename)));
      arg("center", boolean(n->center));
      arg("invert", boolean(n->invert));
      arg("convexity", std::to_string(n->convexity));
    } else if (node.name() == "roof") {
      // no dynamic_cast: RoofNode.cc is only compiled with EXPERIMENTAL and CGAL, so its typeinfo may not exist
      const auto *n = static_cast<const RoofNode *>(&node);
      arg("method", quote(n->method));
      segments(n->fn, n->fa, n->fs);
      arg("convexity", std::to_string(n->convexity));
    }
    // group, union, difference, intersection, hull, fill: no arguments
  }
};

namespace {

void writeLog(std::ostream& out, FgjsonWriter& writer)
{
  out << "\"log\":[";
  bool first = true;
  for (const auto& entry : logged) {
    const auto& m = entry.message;
    if (!first) out << ",";
    first = false;
    out << "{\"group\":" << quote(m.group == message_group::NONE ? "" : getGroupName(m.group)) << ",\"text\":" << quote(m.msg);
    const auto where = writer.at(!m.loc.isNone() ? m.loc : entry.origin);
    if (!where.empty()) out << ",\"at\":" << where;
    out << "}";
  }
  if (dropped) {
    if (!first) out << ",";
    out << "{\"group\":\"WARNING\",\"text\":" << quote(std::to_string(dropped) + " more messages were dropped") << "}";
  }
  out << "]";
}

void writeFiles(std::ostream& out, const FgjsonWriter& writer)
{
  out << "\"files\":[";
  for (size_t i = 0; i < writer.files.size(); ++i) {
    if (i) out << ",";
    out << quote(writer.files[i]);
  }
  out << "]";
}

}  // namespace

void fgjson_collect_messages()
{
  logged.clear();
  dropped = 0;
  message_tap = tap;
}

void export_fgjson(const Tree& tree, const AbstractNode& root, SourceFile *root_file, const fs::path& path,
                   const std::unordered_map<std::string, std::string>& options, std::ostream& out)
{
  FullPrecisionScope full;
  const auto meshOption = options.find("mesh");
  FgjsonWriter writer(out, &tree, meshOption != options.end() && (meshOption->second == "true" || meshOption->second == "1"));
  out << "{\"format\":\"forgent3d-openscad-tree\",\"version\":1,\"tree\":[";
  bool first = true;
  writer.list(root, false, false, first);
  out << "],";

  std::ostringstream params;
  export_param(root_file, path, params);
  out << "\"params\":" << (params.str().empty() ? "null" : params.str()) << ",";

  const auto csg = options.find("csg");
  if (csg != options.end() && (csg->second == "true" || csg->second == "1")) {
    out << "\"csg\":" << quote(tree.getString(root, "\t")) << ",";
  }

  writeLog(out, writer);
  out << ",";
  writeFiles(out, writer);
  out << "}\n";
  message_tap = nullptr;
}

void export_fgjson_failed(std::ostream& out)
{
  FgjsonWriter writer(out);
  out << "{\"format\":\"forgent3d-openscad-tree\",\"version\":1,\"tree\":null,\"params\":null,";
  writeLog(out, writer);
  out << ",";
  writeFiles(out, writer);
  out << "}\n";
  message_tap = nullptr;
}
