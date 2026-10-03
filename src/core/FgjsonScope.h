#pragma once

/*
 * Forgent3D (export_fgjson.cc `scope`): what a user module saw when it was called, for the modules the consumer
 * asked about (`-O fgjson/scope=name,name…`, `*` for every module). The consumer recognises some library modules
 * (BOSL2's cuboid(), cyl()…) and builds their shape exactly from these values instead of from the polygons the
 * module computed; the values are the module's own after its body ran — parameters with defaults applied, then
 * the body's top-level assignments (BOSL2 normalises `edges` and `size` there) —, and `$fn`/`$fa`/`$fs` as they
 * were at the call (an argument of the call wins over the caller's), which tells a library's own facet count from
 * the author's.
 */

#include <memory>
#include <string>
#include <utility>

#include "core/node.h"

class Arguments;
class Context;
class UserModule;

/** A user module's group, carrying its scope as JSON members (`"scope":{…},"specials":{…}`). */
class ScopedGroupNode : public GroupNode
{
public:
  ScopedGroupNode(const ModuleInstantiation *mi, std::string name, std::string scope) : GroupNode(mi, std::move(name)), scope(std::move(scope)) {}
  const std::string scope;
};

/** Whether `-O fgjson/scope=…` asked for this module's scope. */
bool fgjson_scope_wanted(const std::string& module);

/** The specials at the call: `call` is the call's own arguments, `caller` the context it was made in. */
std::string fgjson_call_specials(const Arguments& call, const std::shared_ptr<const Context>& caller);

/** `"scope":{…},"specials":{…}` for `module`, from its context after the body's assignments ran. */
std::string fgjson_module_scope(const UserModule& module, const Context& context, const std::string& specials);
