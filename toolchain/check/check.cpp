// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "toolchain/check/check.h"

#include <variant>

#include "common/check.h"
#include "common/error.h"
#include "common/variant_helpers.h"
#include "common/vlog.h"
#include "toolchain/base/pretty_stack_trace_function.h"
#include "toolchain/check/context.h"
#include "toolchain/check/diagnostic_helpers.h"
#include "toolchain/check/function.h"
#include "toolchain/check/handle.h"
#include "toolchain/check/import.h"
#include "toolchain/check/import_ref.h"
#include "toolchain/diagnostics/diagnostic.h"
#include "toolchain/diagnostics/diagnostic_emitter.h"
#include "toolchain/lex/token_kind.h"
#include "toolchain/parse/node_ids.h"
#include "toolchain/parse/tree.h"
#include "toolchain/parse/tree_node_diagnostic_converter.h"
#include "toolchain/sem_ir/file.h"
#include "toolchain/sem_ir/ids.h"
#include "toolchain/sem_ir/typed_insts.h"

namespace Carbon::Check {

// Handles the transformation of a SemIRLoc to a DiagnosticLoc.
//
// TODO: Move this to diagnostic_helpers.cpp.
class SemIRDiagnosticConverter : public DiagnosticConverter<SemIRLoc> {
 public:
  explicit SemIRDiagnosticConverter(
      const llvm::DenseMap<const SemIR::File*, Parse::NodeLocConverter*>*
          node_converters,
      const SemIR::File* sem_ir)
      : node_converters_(node_converters), sem_ir_(sem_ir) {}

  // Converts an instruction's location to a diagnostic location, which will be
  // the underlying line of code. Adds context for any imports used in the
  // current SemIR to get to the underlying code.
  auto ConvertLoc(SemIRLoc loc, ContextFnT context_fn) const
      -> DiagnosticLoc override {
    // Cursors for the current IR and instruction in that IR.
    const auto* cursor_ir = sem_ir_;
    auto cursor_inst_id = SemIR::InstId::Invalid;

    // Notes an import on the diagnostic and updates cursors to point at the
    // imported IR.
    auto follow_import_ref = [&](SemIR::ImportIRInstId import_ir_inst_id) {
      auto import_ir_inst = cursor_ir->import_ir_insts().Get(import_ir_inst_id);
      const auto& import_ir = cursor_ir->import_irs().Get(import_ir_inst.ir_id);
      auto context_loc = ConvertLocInFile(cursor_ir, import_ir.node_id,
                                          loc.token_only, context_fn);
      CARBON_DIAGNOSTIC(InImport, Note, "In import.");
      context_fn(context_loc, InImport);
      cursor_ir = import_ir.sem_ir;
      cursor_inst_id = import_ir_inst.inst_id;
    };

    // If the location is is an import, follows it and returns nullopt.
    // Otherwise, it's a parse node, so return the final location.
    auto handle_loc = [&](SemIR::LocId loc_id) -> std::optional<DiagnosticLoc> {
      if (loc_id.is_import_ir_inst_id()) {
        follow_import_ref(loc_id.import_ir_inst_id());
        return std::nullopt;
      } else {
        // Parse nodes always refer to the current IR.
        return ConvertLocInFile(cursor_ir, loc_id.node_id(), loc.token_only,
                                context_fn);
      }
    };

    // Handle the base location.
    if (loc.is_inst_id) {
      cursor_inst_id = loc.inst_id;
    } else {
      if (auto diag_loc = handle_loc(loc.loc_id)) {
        return *diag_loc;
      }
      CARBON_CHECK(cursor_inst_id.is_valid()) << "Should have been set";
    }

    while (true) {
      if (cursor_inst_id.is_valid()) {
        auto cursor_inst = cursor_ir->insts().Get(cursor_inst_id);
        if (auto bind_ref = cursor_inst.TryAs<SemIR::BindExport>();
            bind_ref && bind_ref->value_id.is_valid()) {
          cursor_inst_id = bind_ref->value_id;
          continue;
        }

        // If the parse node is valid, use it for the location.
        if (auto loc_id = cursor_ir->insts().GetLocId(cursor_inst_id);
            loc_id.is_valid()) {
          if (auto diag_loc = handle_loc(loc_id)) {
            return *diag_loc;
          }
          continue;
        }

        // If a namespace has an instruction for an import, switch to looking at
        // it.
        if (auto ns = cursor_inst.TryAs<SemIR::Namespace>()) {
          if (ns->import_id.is_valid()) {
            cursor_inst_id = ns->import_id;
            continue;
          }
        }
      }

      // Invalid parse node but not an import; just nothing to point at.
      return ConvertLocInFile(cursor_ir, Parse::NodeId::Invalid, loc.token_only,
                              context_fn);
    }
  }

  auto ConvertArg(llvm::Any arg) const -> llvm::Any override {
    if (auto* name_id = llvm::any_cast<SemIR::NameId>(&arg)) {
      return sem_ir_->names().GetFormatted(*name_id).str();
    }
    if (auto* type_id = llvm::any_cast<SemIR::TypeId>(&arg)) {
      return sem_ir_->StringifyType(*type_id);
    }
    if (auto* typed_int = llvm::any_cast<TypedInt>(&arg)) {
      return llvm::APSInt(typed_int->value,
                          !sem_ir_->types().IsSignedInt(typed_int->type));
    }
    return DiagnosticConverter<SemIRLoc>::ConvertArg(arg);
  }

 private:
  auto ConvertLocInFile(const SemIR::File* sem_ir, Parse::NodeId node_id,
                        bool token_only, ContextFnT context_fn) const
      -> DiagnosticLoc {
    auto it = node_converters_->find(sem_ir);
    CARBON_CHECK(it != node_converters_->end());
    return it->second->ConvertLoc(Parse::NodeLoc(node_id, token_only),
                                  context_fn);
  }

  const llvm::DenseMap<const SemIR::File*, Parse::NodeLocConverter*>*
      node_converters_;
  const SemIR::File* sem_ir_;
};

struct UnitInfo {
  // A given import within the file, with its destination.
  struct Import {
    Parse::Tree::PackagingNames names;
    UnitInfo* unit_info;
  };
  // A file's imports corresponding to a single package, for the map.
  struct PackageImports {
    // Use the constructor so that the SmallVector is only constructed
    // as-needed.
    explicit PackageImports(IdentifierId package_id,
                            Parse::ImportDirectiveId node_id)
        : package_id(package_id), node_id(node_id) {}

    // The identifier of the imported package.
    IdentifierId package_id;
    // The first `import` directive in the file, which declared the package's
    // identifier (even if the import failed). Used for associating diagnostics
    // not specific to a single import.
    Parse::ImportDirectiveId node_id;
    // Whether there's an import that failed to load.
    bool has_load_error = false;
    // The list of valid imports.
    llvm::SmallVector<Import> imports;
  };

  explicit UnitInfo(SemIR::CheckIRId check_ir_id, Unit& unit)
      : check_ir_id(check_ir_id),
        unit(&unit),
        converter(unit.tokens, unit.tokens->source().filename(),
                  unit.parse_tree),
        err_tracker(*unit.consumer),
        emitter(converter, err_tracker) {}

  SemIR::CheckIRId check_ir_id;
  Unit* unit;

  // Emitter information.
  Parse::NodeLocConverter converter;
  ErrorTrackingDiagnosticConsumer err_tracker;
  DiagnosticEmitter<Parse::NodeLoc> emitter;

  // List of the outgoing imports. If a package includes unavailable library
  // imports, it has an entry with has_load_error set. Invalid imports (for
  // example, `import Main;`) aren't added because they won't add identifiers to
  // name lookup.
  llvm::SmallVector<PackageImports> package_imports;

  // A map of the package names to the outgoing imports above.
  llvm::DenseMap<IdentifierId, int32_t> package_imports_map;

  // The remaining number of imports which must be checked before this unit can
  // be processed.
  int32_t imports_remaining = 0;

  // A list of incoming imports. This will be empty for `impl` files, because
  // imports only touch `api` files.
  llvm::SmallVector<UnitInfo*> incoming_imports;

  // The corresponding `api` unit if this is an `impl` file. The entry should
  // also be in the corresponding `PackageImports`.
  UnitInfo* api_for_impl = nullptr;
};

// Imports the current package.
static auto ImportCurrentPackage(Context& context, UnitInfo& unit_info,
                                 int total_ir_count,
                                 SemIR::InstId package_inst_id,
                                 SemIR::TypeId namespace_type_id) -> void {
  // Add imports from the current package.
  auto self_import_map_it =
      unit_info.package_imports_map.find(IdentifierId::Invalid);
  if (self_import_map_it == unit_info.package_imports_map.end()) {
    // Push the scope; there are no names to add.
    context.scope_stack().Push(package_inst_id, SemIR::NameScopeId::Package);
    return;
  }
  UnitInfo::PackageImports& self_import =
      unit_info.package_imports[self_import_map_it->second];

  // Track whether an IR was imported in full, including `export import`. This
  // distinguishes from IRs that are indirectly added without all names being
  // exported to this IR.
  llvm::SmallVector<bool> imported_irs(total_ir_count, false);
  if (self_import.has_load_error) {
    context.name_scopes().Get(SemIR::NameScopeId::Package).has_error = true;
  }

  for (const auto& import : self_import.imports) {
    const auto& import_sem_ir = **import.unit_info->unit->sem_ir;

    auto& imported_ir = imported_irs[import_sem_ir.check_ir_id().index];
    if (!imported_ir) {
      imported_ir = true;

      // Import the IR and its exported imports.
      ImportLibraryFromCurrentPackage(context, namespace_type_id,
                                      import.names.node_id, import_sem_ir,
                                      import.names.is_export);

      for (const auto& indirect_ir : import_sem_ir.import_irs().array_ref()) {
        if (indirect_ir.is_export) {
          auto& imported_indirect_ir =
              imported_irs[indirect_ir.sem_ir->check_ir_id().index];
          if (!imported_indirect_ir) {
            imported_indirect_ir = true;

            ImportLibraryFromCurrentPackage(
                context, namespace_type_id, import.names.node_id,
                *indirect_ir.sem_ir, import.names.is_export);
          } else if (import.names.is_export) {
            // The indirect IR was previously indirectly imported, but it's
            // found through `export import`. We need to mark it for re-export.
            context.import_irs()
                .Get(context.check_ir_map()[indirect_ir.sem_ir->check_ir_id()
                                                .index])
                .is_export = true;
          }
        }
      }
    } else if (import.names.is_export) {
      // The IR was previously indirectly imported, but it's `export import`.
      // We need to mark it -- and transitive `export import`s -- for re-export.
      context.import_irs()
          .Get(context.check_ir_map()[import_sem_ir.check_ir_id().index])
          .is_export = true;

      for (const auto& indirect_ir : import_sem_ir.import_irs().array_ref()) {
        if (indirect_ir.is_export) {
          context.import_irs()
              .Get(context
                       .check_ir_map()[indirect_ir.sem_ir->check_ir_id().index])
              .is_export = true;
        }
      }
    }
  }

  context.scope_stack().Push(
      package_inst_id, SemIR::NameScopeId::Package,
      context.name_scopes().Get(SemIR::NameScopeId::Package).has_error);
}

// Add imports to the root block.
static auto InitPackageScopeAndImports(Context& context, UnitInfo& unit_info,
                                       int total_ir_count) -> void {
  // First create the constant values map for all imported IRs. We'll populate
  // these with mappings for namespaces as we go.
  size_t num_irs = 0;
  for (auto& package_imports : unit_info.package_imports) {
    num_irs += package_imports.imports.size();
  }
  if (!unit_info.api_for_impl) {
    // Leave an empty slot for ImportIRId::ApiForImpl.
    ++num_irs;
  }

  context.import_irs().Reserve(num_irs);
  context.import_ir_constant_values().reserve(num_irs);

  context.check_ir_map().resize(total_ir_count, SemIR::ImportIRId::Invalid);

  // Importing makes many namespaces, so only canonicalize the type once.
  auto namespace_type_id =
      context.GetBuiltinType(SemIR::BuiltinKind::NamespaceType);

  // Define the package scope, with an instruction for `package` expressions to
  // reference.
  auto package_scope_id = context.name_scopes().Add(
      SemIR::InstId::PackageNamespace, SemIR::NameId::PackageNamespace,
      SemIR::NameScopeId::Invalid);
  CARBON_CHECK(package_scope_id == SemIR::NameScopeId::Package);

  auto package_inst_id = context.AddInst(
      {Parse::NodeId::Invalid,
       SemIR::Namespace{namespace_type_id, SemIR::NameScopeId::Package,
                        SemIR::InstId::Invalid}});
  CARBON_CHECK(package_inst_id == SemIR::InstId::PackageNamespace);

  // If there is an implicit `api` import, set it first so that it uses the
  // ImportIRId::ApiForImpl when processed for imports.
  if (unit_info.api_for_impl) {
    SetApiImportIR(
        context,
        {.node_id = context.parse_tree().packaging_directive()->names.node_id,
         .sem_ir = &**unit_info.api_for_impl->unit->sem_ir});
  } else {
    SetApiImportIR(context,
                   {.node_id = Parse::InvalidNodeId(), .sem_ir = nullptr});
  }

  ImportCurrentPackage(context, unit_info, total_ir_count, package_inst_id,
                       namespace_type_id);
  CARBON_CHECK(context.scope_stack().PeekIndex() == ScopeIndex::Package);

  for (auto& package_imports : unit_info.package_imports) {
    if (!package_imports.package_id.is_valid()) {
      // Current package is handled above.
      continue;
    }

    llvm::SmallVector<SemIR::ImportIR> import_irs;
    for (auto import : package_imports.imports) {
      import_irs.push_back({.node_id = import.names.node_id,
                            .sem_ir = &**import.unit_info->unit->sem_ir,
                            .is_export = false});
      CARBON_CHECK(!import.names.is_export)
          << "Imports from other packages can't be exported.";
    }
    ImportLibrariesFromOtherPackage(
        context, namespace_type_id, package_imports.node_id,
        package_imports.package_id, import_irs, package_imports.has_load_error);
  }
}

namespace {
// State used to track the next deferred function definition that we will
// encounter and need to reorder.
class NextDeferredDefinitionCache {
 public:
  explicit NextDeferredDefinitionCache(const Parse::Tree* tree) : tree_(tree) {
    SkipTo(Parse::DeferredDefinitionIndex(0));
  }

  // Set the specified deferred definition index as being the next one that will
  // be encountered.
  auto SkipTo(Parse::DeferredDefinitionIndex next_index) -> void {
    index_ = next_index;
    if (static_cast<std::size_t>(index_.index) ==
        tree_->deferred_definitions().size()) {
      start_id_ = Parse::NodeId::Invalid;
    } else {
      start_id_ = tree_->deferred_definitions().Get(index_).start_id;
    }
  }

  // Returns the index of the next deferred definition to be encountered.
  auto index() const -> Parse::DeferredDefinitionIndex { return index_; }

  // Returns the ID of the start node of the next deferred definition.
  auto start_id() const -> Parse::NodeId { return start_id_; }

 private:
  const Parse::Tree* tree_;
  Parse::DeferredDefinitionIndex index_ =
      Parse::DeferredDefinitionIndex::Invalid;
  Parse::NodeId start_id_ = Parse::NodeId::Invalid;
};
}  // namespace

// Determines whether this node kind is the start of a deferred definition
// scope.
static auto IsStartOfDeferredDefinitionScope(Parse::NodeKind kind) -> bool {
  switch (kind) {
    case Parse::NodeKind::ClassDefinitionStart:
    case Parse::NodeKind::ImplDefinitionStart:
    case Parse::NodeKind::InterfaceDefinitionStart:
    case Parse::NodeKind::NamedConstraintDefinitionStart:
      // TODO: Mixins.
      return true;
    default:
      return false;
  }
}

// Determines whether this node kind is the end of a deferred definition scope.
static auto IsEndOfDeferredDefinitionScope(Parse::NodeKind kind) -> bool {
  switch (kind) {
    case Parse::NodeKind::ClassDefinition:
    case Parse::NodeKind::ImplDefinition:
    case Parse::NodeKind::InterfaceDefinition:
    case Parse::NodeKind::NamedConstraintDefinition:
      // TODO: Mixins.
      return true;
    default:
      return false;
  }
}

namespace {
// A worklist of pending tasks to perform to check deferred function definitions
// in the right order.
class DeferredDefinitionWorklist {
 public:
  // A worklist task that indicates we should check a deferred function
  // definition that we previously skipped.
  struct CheckSkippedDefinition {
    // The definition that we skipped.
    Parse::DeferredDefinitionIndex definition_index;
    // The suspended function.
    SuspendedFunction suspended_fn;
  };

  // A worklist task that indicates we should enter a nested deferred definition
  // scope.
  struct EnterDeferredDefinitionScope {
    // The suspended scope. This is only set once we reach the end of the scope.
    std::optional<DeclNameStack::SuspendedName> suspended_name;
    // Whether this scope is itself within an outer deferred definition scope.
    // If so, we'll delay processing its contents until we reach the end of the
    // enclosing scope. For example:
    //
    // ```
    // class A {
    //   class B {
    //     fn F() -> A { return {}; }
    //   }
    // } // A.B.F is type-checked here, with A complete.
    //
    // fn F() {
    //   class C {
    //     fn G() {}
    //   } // C.G is type-checked here.
    // }
    // ```
    bool in_deferred_definition_scope;
  };

  // A worklist task that indicates we should leave a deferred definition scope.
  struct LeaveDeferredDefinitionScope {
    // Whether this scope is within another deferred definition scope.
    bool in_deferred_definition_scope;
  };

  // A pending type-checking task.
  using Task =
      std::variant<CheckSkippedDefinition, EnterDeferredDefinitionScope,
                   LeaveDeferredDefinitionScope>;

  explicit DeferredDefinitionWorklist(llvm::raw_ostream* vlog_stream)
      : vlog_stream_(vlog_stream) {
    // See declaration of `worklist_`.
    worklist_.reserve(64);
  }

  static constexpr llvm::StringLiteral VlogPrefix =
      "DeferredDefinitionWorklist ";

  // Suspend the current function definition and push a task onto the worklist
  // to finish it later.
  auto SuspendFunctionAndPush(Context& context,
                              Parse::DeferredDefinitionIndex index,
                              Parse::FunctionDefinitionStartId node_id)
      -> void {
    worklist_.push_back(CheckSkippedDefinition{
        index, HandleFunctionDefinitionSuspend(context, node_id)});
    CARBON_VLOG() << VlogPrefix << "Push CheckSkippedDefinition " << index.index
                  << "\n";
  }

  // Push a task to re-enter a function scope, so that functions defined within
  // it are type-checked in the right context.
  auto PushEnterDeferredDefinitionScope(Context& context) -> void {
    bool nested = !enclosing_scopes_.empty() &&
                  enclosing_scopes_.back().scope_index ==
                      context.decl_name_stack().PeekInitialScopeIndex();
    enclosing_scopes_.push_back(
        {.worklist_start_index = worklist_.size(),
         .scope_index = context.scope_stack().PeekIndex()});
    worklist_.push_back(EnterDeferredDefinitionScope{std::nullopt, nested});
    CARBON_VLOG() << VlogPrefix << "Push EnterDeferredDefinitionScope "
                  << (nested ? "(nested)" : "(non-nested)") << "\n";
  }

  // Suspend the current deferred definition scope, which is finished but still
  // on the decl_name_stack, and push a task to leave the scope when we're
  // type-checking deferred definitions. Returns `true` if the current list of
  // deferred definitions should be type-checked immediately.
  auto SuspendFinishedScopeAndPush(Context& context) -> bool;

  // Pop the next task off the worklist.
  auto Pop() -> Task {
    if (vlog_stream_) {
      VariantMatch(
          worklist_.back(),
          [&](CheckSkippedDefinition& definition) {
            CARBON_VLOG() << VlogPrefix << "Handle CheckSkippedDefinition "
                          << definition.definition_index.index << "\n";
          },
          [&](EnterDeferredDefinitionScope& enter) {
            CARBON_CHECK(enter.in_deferred_definition_scope);
            CARBON_VLOG() << VlogPrefix
                          << "Handle EnterDeferredDefinitionScope (nested)\n";
          },
          [&](LeaveDeferredDefinitionScope& leave) {
            bool nested = leave.in_deferred_definition_scope;
            CARBON_VLOG() << VlogPrefix
                          << "Handle LeaveDeferredDefinitionScope "
                          << (nested ? "(nested)" : "(non-nested)") << "\n";
          });
    }

    return worklist_.pop_back_val();
  }

  // CHECK that the work list has no further work.
  auto VerifyEmpty() {
    CARBON_CHECK(worklist_.empty() && enclosing_scopes_.empty())
        << "Tasks left behind on worklist.";
  }

 private:
  llvm::raw_ostream* vlog_stream_;

  // A worklist of type-checking tasks we'll need to do later.
  //
  // Don't allocate any inline storage here. A Task is fairly large, so we never
  // want this to live on the stack. Instead, we reserve space in the
  // constructor for a fairly large number of deferred definitions.
  llvm::SmallVector<Task, 0> worklist_;

  // A deferred definition scope that is currently still open.
  struct EnclosingScope {
    // The index in worklist_ of the EnterDeferredDefinitionScope task.
    size_t worklist_start_index;
    // The corresponding lexical scope index.
    ScopeIndex scope_index;
  };

  // The deferred definition scopes enclosing the current checking actions.
  llvm::SmallVector<EnclosingScope> enclosing_scopes_;
};
}  // namespace

auto DeferredDefinitionWorklist::SuspendFinishedScopeAndPush(Context& context)
    -> bool {
  auto start_index = enclosing_scopes_.pop_back_val().worklist_start_index;

  // If we've not found any deferred definitions in this scope, clean up the
  // stack.
  if (start_index == worklist_.size() - 1) {
    context.decl_name_stack().PopScope();
    worklist_.pop_back();
    CARBON_VLOG() << VlogPrefix << "Pop EnterDeferredDefinitionScope (empty)\n";
    return false;
  }

  // If we're finishing a nested deferred definition scope, keep track of that
  // but don't type-check deferred definitions now.
  auto& enter_scope = get<EnterDeferredDefinitionScope>(worklist_[start_index]);
  if (enter_scope.in_deferred_definition_scope) {
    // This is a nested deferred definition scope. Suspend the inner scope so we
    // can restore it when we come to type-check the deferred definitions.
    enter_scope.suspended_name = context.decl_name_stack().Suspend();

    // Enqueue a task to leave the nested scope.
    worklist_.push_back(
        LeaveDeferredDefinitionScope{.in_deferred_definition_scope = true});
    CARBON_VLOG() << VlogPrefix
                  << "Push LeaveDeferredDefinitionScope (nested)\n";
    return false;
  }

  // We're at the end of a non-nested deferred definition scope. Prepare to
  // start checking deferred definitions. Enqueue a task to leave this outer
  // scope and end checking deferred definitions.
  worklist_.push_back(
      LeaveDeferredDefinitionScope{.in_deferred_definition_scope = false});
  CARBON_VLOG() << VlogPrefix
                << "Push LeaveDeferredDefinitionScope (non-nested)\n";

  // We'll process the worklist in reverse index order, so reverse the part of
  // it we're about to execute so we run our tasks in the order in which they
  // were pushed.
  std::reverse(worklist_.begin() + start_index, worklist_.end());

  // Pop the `EnterDeferredDefinitionScope` that's now on the end of the
  // worklist. We stay in that scope rather than suspending then immediately
  // resuming it.
  CARBON_CHECK(
      holds_alternative<EnterDeferredDefinitionScope>(worklist_.back()))
      << "Unexpected task in worklist.";
  worklist_.pop_back();
  CARBON_VLOG() << VlogPrefix
                << "Handle EnterDeferredDefinitionScope (non-nested)\n";
  return true;
}

namespace {
// A traversal of the node IDs in the parse tree, in the order in which we need
// to check them.
class NodeIdTraversal {
 public:
  explicit NodeIdTraversal(Context& context, llvm::raw_ostream* vlog_stream)
      : context_(context),
        next_deferred_definition_(&context.parse_tree()),
        worklist_(vlog_stream) {
    chunks_.push_back(
        {.it = context.parse_tree().postorder().begin(),
         .end = context.parse_tree().postorder().end(),
         .next_definition = Parse::DeferredDefinitionIndex::Invalid});
  }

  // Finds the next `NodeId` to type-check. Returns nullopt if the traversal is
  // complete.
  auto Next() -> std::optional<Parse::NodeId>;

  // Performs any processing necessary after we type-check a node.
  auto Handle(Parse::NodeKind parse_kind) -> void {
    // When we reach the start of a deferred definition scope, add a task to the
    // worklist to check future skipped definitions in the new context.
    if (IsStartOfDeferredDefinitionScope(parse_kind)) {
      worklist_.PushEnterDeferredDefinitionScope(context_);
    }

    // When we reach the end of a deferred definition scope, add a task to the
    // worklist to leave the scope. If this is not a nested scope, start
    // checking the deferred definitions now.
    if (IsEndOfDeferredDefinitionScope(parse_kind)) {
      chunks_.back().checking_deferred_definitions =
          worklist_.SuspendFinishedScopeAndPush(context_);
    }
  }

 private:
  // A chunk of the parse tree that we need to type-check.
  struct Chunk {
    Parse::Tree::PostorderIterator it;
    Parse::Tree::PostorderIterator end;
    // The next definition that will be encountered after this chunk completes.
    Parse::DeferredDefinitionIndex next_definition;
    // Whether we are currently checking deferred definitions, rather than the
    // tokens of this chunk. If so, we'll pull tasks off `worklist` and execute
    // them until we're done with this batch of deferred definitions. Otherwise,
    // we'll pull node IDs from `*it` until it reaches `end`.
    bool checking_deferred_definitions = false;
  };

  // Re-enter a nested deferred definition scope.
  auto PerformTask(
      DeferredDefinitionWorklist::EnterDeferredDefinitionScope&& enter)
      -> void {
    CARBON_CHECK(enter.suspended_name)
        << "Entering a scope with no suspension information.";
    context_.decl_name_stack().Restore(std::move(*enter.suspended_name));
  }

  // Leave a nested or top-level deferred definition scope.
  auto PerformTask(
      DeferredDefinitionWorklist::LeaveDeferredDefinitionScope&& leave)
      -> void {
    if (!leave.in_deferred_definition_scope) {
      // We're done with checking deferred definitions.
      chunks_.back().checking_deferred_definitions = false;
    }
    context_.decl_name_stack().PopScope();
  }

  // Resume checking a deferred definition.
  auto PerformTask(
      DeferredDefinitionWorklist::CheckSkippedDefinition&& parse_definition)
      -> void {
    auto& [definition_index, suspended_fn] = parse_definition;
    const auto& definition_info =
        context_.parse_tree().deferred_definitions().Get(definition_index);
    HandleFunctionDefinitionResume(context_, definition_info.start_id,
                                   std::move(suspended_fn));
    chunks_.push_back(
        {.it = context_.parse_tree().postorder(definition_info.start_id).end(),
         .end = context_.parse_tree()
                    .postorder(definition_info.definition_id)
                    .end(),
         .next_definition = next_deferred_definition_.index()});
    ++definition_index.index;
    next_deferred_definition_.SkipTo(definition_index);
  }

  Context& context_;
  NextDeferredDefinitionCache next_deferred_definition_;
  DeferredDefinitionWorklist worklist_;
  llvm::SmallVector<Chunk> chunks_;
};
}  // namespace

auto NodeIdTraversal::Next() -> std::optional<Parse::NodeId> {
  while (true) {
    // If we're checking deferred definitions, find the next definition we
    // should check, restore its suspended state, and add a corresponding
    // `Chunk` to the top of the chunk list.
    if (chunks_.back().checking_deferred_definitions) {
      std::visit(
          [&](auto&& task) { PerformTask(std::forward<decltype(task)>(task)); },
          worklist_.Pop());
      continue;
    }

    // If we're not checking deferred definitions, produce the next parse node
    // for this chunk. If we've run out of parse nodes, we're done with this
    // chunk of the parse tree.
    if (chunks_.back().it == chunks_.back().end) {
      auto old_chunk = chunks_.pop_back_val();

      // If we're out of chunks, then we're done entirely.
      if (chunks_.empty()) {
        worklist_.VerifyEmpty();
        return std::nullopt;
      }

      next_deferred_definition_.SkipTo(old_chunk.next_definition);
      continue;
    }

    auto node_id = *chunks_.back().it;

    // If we've reached the start of a deferred definition, skip to the end of
    // it, and track that we need to check it later.
    if (node_id == next_deferred_definition_.start_id()) {
      const auto& definition_info =
          context_.parse_tree().deferred_definitions().Get(
              next_deferred_definition_.index());
      worklist_.SuspendFunctionAndPush(context_,
                                       next_deferred_definition_.index(),
                                       definition_info.start_id);

      // Continue type-checking the parse tree after the end of the definition.
      chunks_.back().it =
          context_.parse_tree().postorder(definition_info.definition_id).end();
      next_deferred_definition_.SkipTo(definition_info.next_definition_index);
      continue;
    }

    ++chunks_.back().it;
    return node_id;
  }
}

// Loops over all nodes in the tree. On some errors, this may return early,
// for example if an unrecoverable state is encountered.
// NOLINTNEXTLINE(readability-function-size)
static auto ProcessNodeIds(Context& context, llvm::raw_ostream* vlog_stream,
                           ErrorTrackingDiagnosticConsumer& err_tracker,
                           Parse::NodeLocConverter* converter) -> bool {
  NodeIdTraversal traversal(context, vlog_stream);

  Parse::NodeId node_id = Parse::NodeId::Invalid;

  // On crash, report which token we were handling.
  PrettyStackTraceFunction node_dumper([&](llvm::raw_ostream& output) {
    auto loc = converter->ConvertLoc(
        node_id, [](DiagnosticLoc, const Internal::DiagnosticBase<>&) {});
    loc.FormatLocation(output);
    output << ": Check::Handle" << context.parse_tree().node_kind(node_id)
           << "\n";
    loc.FormatSnippet(output);
  });

  while (auto maybe_node_id = traversal.Next()) {
    node_id = *maybe_node_id;
    auto parse_kind = context.parse_tree().node_kind(node_id);

    switch (parse_kind) {
#define CARBON_PARSE_NODE_KIND(Name)                                         \
  case Parse::NodeKind::Name: {                                              \
    if (!Check::Handle##Name(context, Parse::Name##Id(node_id))) {           \
      CARBON_CHECK(err_tracker.seen_error())                                 \
          << "Handle" #Name " returned false without printing a diagnostic"; \
      return false;                                                          \
    }                                                                        \
    break;                                                                   \
  }
#include "toolchain/parse/node_kind.def"
    }

    traversal.Handle(parse_kind);
  }
  return true;
}

// Produces and checks the IR for the provided Parse::Tree.
static auto CheckParseTree(
    llvm::DenseMap<const SemIR::File*, Parse::NodeLocConverter*>*
        node_converters,
    UnitInfo& unit_info, int total_ir_count, llvm::raw_ostream* vlog_stream)
    -> void {
  unit_info.unit->sem_ir->emplace(
      unit_info.check_ir_id, *unit_info.unit->value_stores,
      unit_info.unit->tokens->source().filename().str());

  // For ease-of-access.
  SemIR::File& sem_ir = **unit_info.unit->sem_ir;
  CARBON_CHECK(node_converters->insert({&sem_ir, &unit_info.converter}).second);

  SemIRDiagnosticConverter converter(node_converters, &sem_ir);
  Context::DiagnosticEmitter emitter(converter, unit_info.err_tracker);
  Context context(*unit_info.unit->tokens, emitter, *unit_info.unit->parse_tree,
                  sem_ir, vlog_stream);
  PrettyStackTraceFunction context_dumper(
      [&](llvm::raw_ostream& output) { context.PrintForStackDump(output); });

  // Add a block for the file.
  context.inst_block_stack().Push();

  InitPackageScopeAndImports(context, unit_info, total_ir_count);

  // Import all impls declared in imports.
  // TODO: Do this selectively when we see an impl query.
  ImportImpls(context);

  if (!ProcessNodeIds(context, vlog_stream, unit_info.err_tracker,
                      &unit_info.converter)) {
    context.sem_ir().set_has_errors(true);
    return;
  }

  // Pop information for the file-level scope.
  sem_ir.set_top_inst_block_id(context.inst_block_stack().Pop());
  context.scope_stack().Pop();
  context.FinalizeExports();
  context.FinalizeGlobalInit();

  context.VerifyOnFinish();

  sem_ir.set_has_errors(unit_info.err_tracker.seen_error());

#ifndef NDEBUG
  if (auto verify = sem_ir.Verify(); !verify.ok()) {
    CARBON_FATAL() << sem_ir << "Built invalid semantics IR: " << verify.error()
                   << "\n";
  }
#endif
}

// The package and library names, used as map keys.
using ImportKey = std::pair<llvm::StringRef, llvm::StringRef>;

// Returns a key form of the package object. file_package_id is only used for
// imports, not the main package directive; as a consequence, it will be invalid
// for the main package directive.
static auto GetImportKey(UnitInfo& unit_info, IdentifierId file_package_id,
                         Parse::Tree::PackagingNames names) -> ImportKey {
  auto* stores = unit_info.unit->value_stores;
  llvm::StringRef package_name =
      names.package_id.is_valid()  ? stores->identifiers().Get(names.package_id)
      : file_package_id.is_valid() ? stores->identifiers().Get(file_package_id)
                                   : "";
  llvm::StringRef library_name =
      names.library_id.is_valid()
          ? stores->string_literal_values().Get(names.library_id)
          : "";
  return {package_name, library_name};
}

static constexpr llvm::StringLiteral ExplicitMainName = "Main";

static auto RenderImportKey(ImportKey import_key) -> std::string {
  if (import_key.first.empty()) {
    import_key.first = ExplicitMainName;
  }
  if (import_key.second.empty()) {
    return import_key.first.str();
  }
  return llvm::formatv("{0}//{1}", import_key.first, import_key.second).str();
}

// Marks an import as required on both the source and target file.
//
// The ID comparisons between the import and unit are okay because they both
// come from the same file.
static auto TrackImport(
    llvm::DenseMap<ImportKey, UnitInfo*>& api_map,
    llvm::DenseMap<ImportKey, Parse::NodeId>* explicit_import_map,
    UnitInfo& unit_info, Parse::Tree::PackagingNames import) -> void {
  const auto& packaging = unit_info.unit->parse_tree->packaging_directive();

  IdentifierId file_package_id =
      packaging ? packaging->names.package_id : IdentifierId::Invalid;
  auto import_key = GetImportKey(unit_info, file_package_id, import);

  // True if the import has `Main` as the package name, even if it comes from
  // the file's packaging (diagnostics may differentiate).
  bool is_explicit_main = import_key.first == ExplicitMainName;

  // Explicit imports need more validation than implicit ones. We try to do
  // these in an order of imports that should be removed, followed by imports
  // that might be valid with syntax fixes.
  if (explicit_import_map) {
    // Diagnose redundant imports.
    if (auto [insert_it, success] =
            explicit_import_map->insert({import_key, import.node_id});
        !success) {
      CARBON_DIAGNOSTIC(RepeatedImport, Error,
                        "Library imported more than once.");
      CARBON_DIAGNOSTIC(FirstImported, Note, "First import here.");
      unit_info.emitter.Build(import.node_id, RepeatedImport)
          .Note(insert_it->second, FirstImported)
          .Emit();
      return;
    }

    // True if the file's package is implicitly `Main` (by omitting an explicit
    // package name).
    bool is_file_implicit_main =
        !packaging || !packaging->names.package_id.is_valid();
    // True if the import is using implicit "current package" syntax (by
    // omitting an explicit package name).
    bool is_import_implicit_current_package = !import.package_id.is_valid();
    // True if the import is using `default` library syntax.
    bool is_import_default_library = !import.library_id.is_valid();
    // True if the import and file point at the same package, even by
    // incorrectly specifying the current package name to `import`.
    bool is_same_package = is_import_implicit_current_package ||
                           import.package_id == file_package_id;
    // True if the import points at the same library as the file's library.
    bool is_same_library =
        is_same_package &&
        (packaging ? import.library_id == packaging->names.library_id
                   : is_import_default_library);

    // Diagnose explicit imports of the same library, whether from `api` or
    // `impl`.
    if (is_same_library) {
      CARBON_DIAGNOSTIC(ExplicitImportApi, Error,
                        "Explicit import of `api` from `impl` file is "
                        "redundant with implicit import.");
      CARBON_DIAGNOSTIC(ImportSelf, Error, "File cannot import itself.");
      bool is_impl =
          !packaging || packaging->api_or_impl == Parse::Tree::ApiOrImpl::Impl;
      unit_info.emitter.Emit(import.node_id,
                             is_impl ? ExplicitImportApi : ImportSelf);
      return;
    }

    // Diagnose explicit imports of `Main//default`. There is no `api` for it.
    // This lets other diagnostics handle explicit `Main` package naming.
    if (is_file_implicit_main && is_import_implicit_current_package &&
        is_import_default_library) {
      CARBON_DIAGNOSTIC(ImportMainDefaultLibrary, Error,
                        "Cannot import `Main//default`.");
      unit_info.emitter.Emit(import.node_id, ImportMainDefaultLibrary);

      return;
    }

    if (!is_import_implicit_current_package) {
      // Diagnose explicit imports of the same package that use the package
      // name.
      if (is_same_package || (is_file_implicit_main && is_explicit_main)) {
        CARBON_DIAGNOSTIC(
            ImportCurrentPackageByName, Error,
            "Imports from the current package must omit the package name.");
        unit_info.emitter.Emit(import.node_id, ImportCurrentPackageByName);
        return;
      }

      // Diagnose explicit imports from `Main`.
      if (is_explicit_main) {
        CARBON_DIAGNOSTIC(ImportMainPackage, Error,
                          "Cannot import `Main` from other packages.");
        unit_info.emitter.Emit(import.node_id, ImportMainPackage);
        return;
      }
    }
  } else if (is_explicit_main) {
    // An implicit import with an explicit `Main` occurs when a `package` rule
    // has bad syntax, which will have been diagnosed when building the API map.
    // As a consequence, we return silently.
    return;
  }

  // Get the package imports, or create them if this is the first.
  auto [package_imports_map_it, is_inserted] =
      unit_info.package_imports_map.insert(
          {import.package_id, unit_info.package_imports.size()});
  if (is_inserted) {
    unit_info.package_imports.push_back(
        UnitInfo::PackageImports(import.package_id, import.node_id));
  }
  UnitInfo::PackageImports& package_imports =
      unit_info.package_imports[package_imports_map_it->second];

  if (auto api = api_map.find(import_key); api != api_map.end()) {
    // Add references between the file and imported api.
    package_imports.imports.push_back({import, api->second});
    ++unit_info.imports_remaining;
    api->second->incoming_imports.push_back(&unit_info);

    // If this is the implicit import, note we have it.
    if (!explicit_import_map) {
      CARBON_CHECK(!unit_info.api_for_impl);
      unit_info.api_for_impl = api->second;
    }
  } else {
    // The imported api is missing.
    package_imports.has_load_error = true;
    CARBON_DIAGNOSTIC(LibraryApiNotFound, Error,
                      "Corresponding API for '{0}' not found.", std::string);
    CARBON_DIAGNOSTIC(ImportNotFound, Error, "Imported API '{0}' not found.",
                      std::string);
    unit_info.emitter.Emit(
        import.node_id,
        explicit_import_map ? ImportNotFound : LibraryApiNotFound,
        RenderImportKey(import_key));
  }
}

// Builds a map of `api` files which might be imported. Also diagnoses issues
// related to the packaging because the strings are loaded as part of getting
// the ImportKey (which we then do for `impl` files too).
static auto BuildApiMapAndDiagnosePackaging(
    llvm::SmallVector<UnitInfo, 0>& unit_infos)
    -> llvm::DenseMap<ImportKey, UnitInfo*> {
  llvm::DenseMap<ImportKey, UnitInfo*> api_map;
  for (auto& unit_info : unit_infos) {
    const auto& packaging = unit_info.unit->parse_tree->packaging_directive();
    // An import key formed from the `package` or `library` directive. Or, for
    // Main//default, a placeholder key.
    auto import_key = packaging ? GetImportKey(unit_info, IdentifierId::Invalid,
                                               packaging->names)
                                // Construct a boring key for Main//default.
                                : ImportKey{"", ""};

    // Diagnose explicit `Main` uses before they become marked as possible
    // APIs.
    if (import_key.first == ExplicitMainName) {
      CARBON_DIAGNOSTIC(ExplicitMainPackage, Error,
                        "`Main//default` must omit `package` directive.");
      CARBON_DIAGNOSTIC(ExplicitMainLibrary, Error,
                        "Use `library` directive in `Main` package libraries.");
      unit_info.emitter.Emit(packaging->names.node_id,
                             import_key.second.empty() ? ExplicitMainPackage
                                                       : ExplicitMainLibrary);
      continue;
    }

    bool is_impl =
        packaging && packaging->api_or_impl == Parse::Tree::ApiOrImpl::Impl;

    // Add to the `api` map and diagnose duplicates. This occurs before the
    // file extension check because we might emit both diagnostics in situations
    // where the user forgets (or has syntax errors with) a package line
    // multiple times.
    if (!is_impl) {
      auto [entry, success] = api_map.insert({import_key, &unit_info});
      if (!success) {
        llvm::StringRef prev_filename =
            entry->second->unit->tokens->source().filename();
        if (packaging) {
          CARBON_DIAGNOSTIC(DuplicateLibraryApi, Error,
                            "Library's API previously provided by `{0}`.",
                            std::string);
          unit_info.emitter.Emit(packaging->names.node_id, DuplicateLibraryApi,
                                 prev_filename.str());
        } else {
          CARBON_DIAGNOSTIC(DuplicateMainApi, Error,
                            "Main//default previously provided by `{0}`.",
                            std::string);
          // Use the invalid node because there's no node to associate with.
          unit_info.emitter.Emit(Parse::NodeId::Invalid, DuplicateMainApi,
                                 prev_filename.str());
        }
      }
    }

    // Validate file extensions. Note imports rely the packaging directive, not
    // the extension. If the input is not a regular file, for example because it
    // is stdin, no filename checking is performed.
    if (unit_info.unit->tokens->source().is_regular_file()) {
      auto filename = unit_info.unit->tokens->source().filename();
      static constexpr llvm::StringLiteral ApiExt = ".carbon";
      static constexpr llvm::StringLiteral ImplExt = ".impl.carbon";
      bool is_api_with_impl_ext = !is_impl && filename.ends_with(ImplExt);
      auto want_ext = is_impl ? ImplExt : ApiExt;
      if (is_api_with_impl_ext || !filename.ends_with(want_ext)) {
        CARBON_DIAGNOSTIC(IncorrectExtension, Error,
                          "File extension of `{0}` required for `{1}`.",
                          llvm::StringLiteral, Lex::TokenKind);
        auto diag = unit_info.emitter.Build(
            packaging ? packaging->names.node_id : Parse::NodeId::Invalid,
            IncorrectExtension, want_ext,
            is_impl ? Lex::TokenKind::Impl : Lex::TokenKind::Api);
        if (is_api_with_impl_ext) {
          CARBON_DIAGNOSTIC(IncorrectExtensionImplNote, Note,
                            "File extension of `{0}` only allowed for `{1}`.",
                            llvm::StringLiteral, Lex::TokenKind);
          diag.Note(Parse::NodeId::Invalid, IncorrectExtensionImplNote, ImplExt,
                    Lex::TokenKind::Impl);
        }
        diag.Emit();
      }
    }
  }
  return api_map;
}

auto CheckParseTrees(llvm::MutableArrayRef<Unit> units, bool prelude_import,
                     llvm::raw_ostream* vlog_stream) -> void {
  // Prepare diagnostic emitters in case we run into issues during package
  // checking.
  //
  // UnitInfo is big due to its SmallVectors, so we default to 0 on the stack.
  llvm::SmallVector<UnitInfo, 0> unit_infos;
  unit_infos.reserve(units.size());
  for (auto [i, unit] : llvm::enumerate(units)) {
    unit_infos.emplace_back(SemIR::CheckIRId(i), unit);
  }

  llvm::DenseMap<ImportKey, UnitInfo*> api_map =
      BuildApiMapAndDiagnosePackaging(unit_infos);

  // Mark down imports for all files.
  llvm::SmallVector<UnitInfo*> ready_to_check;
  ready_to_check.reserve(units.size());
  for (auto& unit_info : unit_infos) {
    const auto& packaging = unit_info.unit->parse_tree->packaging_directive();
    if (packaging && packaging->api_or_impl == Parse::Tree::ApiOrImpl::Impl) {
      // An `impl` has an implicit import of its `api`.
      auto implicit_names = packaging->names;
      implicit_names.package_id = IdentifierId::Invalid;
      TrackImport(api_map, nullptr, unit_info, implicit_names);
    }

    llvm::DenseMap<ImportKey, Parse::NodeId> explicit_import_map;

    // Add the prelude import. It's added to explicit_import_map so that it can
    // conflict with an explicit import of the prelude.
    // TODO: Add --no-prelude-import for `/no_prelude/` subdirs.
    IdentifierId core_ident_id =
        unit_info.unit->value_stores->identifiers().Add("Core");
    if (prelude_import &&
        !(packaging && packaging->names.package_id == core_ident_id)) {
      auto prelude_id =
          unit_info.unit->value_stores->string_literal_values().Add("prelude");
      TrackImport(api_map, &explicit_import_map, unit_info,
                  {.node_id = Parse::InvalidNodeId(),
                   .package_id = core_ident_id,
                   .library_id = prelude_id});
    }

    for (const auto& import : unit_info.unit->parse_tree->imports()) {
      TrackImport(api_map, &explicit_import_map, unit_info, import);
    }

    // If there were no imports, mark the file as ready to check for below.
    if (unit_info.imports_remaining == 0) {
      ready_to_check.push_back(&unit_info);
    }
  }

  llvm::DenseMap<const SemIR::File*, Parse::NodeLocConverter*> node_converters;

  // Check everything with no dependencies. Earlier entries with dependencies
  // will be checked as soon as all their dependencies have been checked.
  for (int check_index = 0;
       check_index < static_cast<int>(ready_to_check.size()); ++check_index) {
    auto* unit_info = ready_to_check[check_index];
    CheckParseTree(&node_converters, *unit_info, units.size(), vlog_stream);
    for (auto* incoming_import : unit_info->incoming_imports) {
      --incoming_import->imports_remaining;
      if (incoming_import->imports_remaining == 0) {
        ready_to_check.push_back(incoming_import);
      }
    }
  }

  // If there are still units with remaining imports, it means there's a
  // dependency loop.
  if (ready_to_check.size() < unit_infos.size()) {
    // Go through units and mask out unevaluated imports. This breaks everything
    // associated with a loop equivalently, whether it's part of it or depending
    // on a part of it.
    // TODO: Better identify cycles, maybe try to untangle them.
    for (auto& unit_info : unit_infos) {
      if (unit_info.imports_remaining > 0) {
        for (auto& package_imports : unit_info.package_imports) {
          for (auto* import_it = package_imports.imports.begin();
               import_it != package_imports.imports.end();) {
            if (*import_it->unit_info->unit->sem_ir) {
              // The import is checked, so continue.
              ++import_it;
            } else {
              // The import hasn't been checked, indicating a cycle.
              CARBON_DIAGNOSTIC(ImportCycleDetected, Error,
                                "Import cannot be used due to a cycle. Cycle "
                                "must be fixed to import.");
              unit_info.emitter.Emit(import_it->names.node_id,
                                     ImportCycleDetected);
              // Make this look the same as an import which wasn't found.
              package_imports.has_load_error = true;
              if (unit_info.api_for_impl == import_it->unit_info) {
                unit_info.api_for_impl = nullptr;
              }
              import_it = package_imports.imports.erase(import_it);
            }
          }
        }
      }
    }

    // Check the remaining file contents, which are probably broken due to
    // incomplete imports.
    for (auto& unit_info : unit_infos) {
      if (unit_info.imports_remaining > 0) {
        CheckParseTree(&node_converters, unit_info, units.size(), vlog_stream);
      }
    }
  }
}

}  // namespace Carbon::Check
