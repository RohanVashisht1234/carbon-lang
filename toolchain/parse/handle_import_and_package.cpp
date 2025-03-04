// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "toolchain/base/value_store.h"
#include "toolchain/lex/tokenized_buffer.h"
#include "toolchain/parse/context.h"
#include "toolchain/parse/node_ids.h"
#include "toolchain/parse/node_kind.h"

namespace Carbon::Parse {

// Provides common error exiting logic that skips to the semi, if present.
static auto OnParseError(Context& context, Context::StateStackEntry state,
                         NodeKind directive) -> void {
  return context.AddNode(directive, context.SkipPastLikelyEnd(state.token),
                         state.subtree_start, /*has_error=*/true);
}

// Handles parsing of the library name. Returns the name's ID on success, which
// may be invalid for `default`.
static auto HandleLibraryName(Context& context, bool accept_default)
    -> std::optional<StringLiteralValueId> {
  if (auto library_name_token =
          context.ConsumeIf(Lex::TokenKind::StringLiteral)) {
    context.AddLeafNode(NodeKind::LibraryName, *library_name_token);
    return context.tokens().GetStringLiteralValue(*library_name_token);
  }

  if (accept_default) {
    if (auto default_token = context.ConsumeIf(Lex::TokenKind::Default)) {
      context.AddLeafNode(NodeKind::DefaultLibrary, *default_token);
      return StringLiteralValueId::Invalid;
    }
  }

  CARBON_DIAGNOSTIC(
      ExpectedLibraryNameOrDefault, Error,
      "Expected `default` or a string literal to specify the library name.");
  CARBON_DIAGNOSTIC(ExpectedLibraryName, Error,
                    "Expected a string literal to specify the library name.");
  context.emitter().Emit(*context.position(), accept_default
                                                  ? ExpectedLibraryNameOrDefault
                                                  : ExpectedLibraryName);
  return std::nullopt;
}

// Returns whether `api` or `impl` is provided, or prints an error and returns
// nullopt.
static auto HandleApiOrImpl(Context& context)
    -> std::optional<Tree::ApiOrImpl> {
  switch (context.PositionKind()) {
    case Lex::TokenKind::Api: {
      context.AddLeafNode(NodeKind::PackageApi,
                          context.ConsumeChecked(Lex::TokenKind::Api));
      return Tree::ApiOrImpl::Api;
      break;
    }
    case Lex::TokenKind::Impl: {
      context.AddLeafNode(NodeKind::PackageImpl,
                          context.ConsumeChecked(Lex::TokenKind::Impl));
      return Tree::ApiOrImpl::Impl;
      break;
    }
    default: {
      CARBON_DIAGNOSTIC(ExpectedApiOrImpl, Error, "Expected `api` or `impl`.");
      context.emitter().Emit(*context.position(), ExpectedApiOrImpl);
      return std::nullopt;
    }
  }
}

// Handles everything after the directive's introducer.
static auto HandleDirectiveContent(Context& context,
                                   Context::StateStackEntry state,
                                   NodeKind directive, bool is_export,
                                   llvm::function_ref<void()> on_parse_error)
    -> void {
  Tree::PackagingNames names{
      .node_id = ImportDirectiveId(NodeId(state.subtree_start)),
      .is_export = is_export};
  if (directive != NodeKind::LibraryDirective) {
    if (auto package_name_token =
            context.ConsumeIf(Lex::TokenKind::Identifier)) {
      if (names.is_export) {
        names.is_export = false;
        state.has_error = true;

        CARBON_DIAGNOSTIC(ExportImportPackage, Error,
                          "`export` cannot be used when importing a package.");
        context.emitter().Emit(*package_name_token, ExportImportPackage);
      }
      names.package_id = context.tokens().GetIdentifier(*package_name_token);
      context.AddLeafNode(NodeKind::PackageName, *package_name_token);
    } else if (directive == NodeKind::PackageDirective ||
               !context.PositionIs(Lex::TokenKind::Library)) {
      CARBON_DIAGNOSTIC(ExpectedIdentifierAfterPackage, Error,
                        "Expected identifier after `package`.");
      CARBON_DIAGNOSTIC(ExpectedIdentifierAfterImport, Error,
                        "Expected identifier or `library` after `import`.");
      context.emitter().Emit(*context.position(),
                             directive == NodeKind::PackageDirective
                                 ? ExpectedIdentifierAfterPackage
                                 : ExpectedIdentifierAfterImport);
      on_parse_error();
      return;
    }
  }

  // Parse the optional library keyword.
  bool accept_default = !names.package_id.is_valid();
  if (directive == NodeKind::LibraryDirective) {
    auto library_id = HandleLibraryName(context, accept_default);
    if (!library_id) {
      on_parse_error();
      return;
    }
    names.library_id = *library_id;
  } else {
    auto next_kind = context.PositionKind();
    if (next_kind == Lex::TokenKind::Library) {
      auto library_token = context.ConsumeChecked(Lex::TokenKind::Library);
      auto library_subtree_start = context.tree().size();
      auto library_id = HandleLibraryName(context, accept_default);
      if (!library_id) {
        on_parse_error();
        return;
      }
      names.library_id = *library_id;
      context.AddNode(NodeKind::LibrarySpecifier, library_token,
                      library_subtree_start,
                      /*has_error=*/false);
    } else if (next_kind == Lex::TokenKind::StringLiteral ||
               (accept_default && next_kind == Lex::TokenKind::Default)) {
      // If we come across a string literal and we didn't parse `library
      // "..."` yet, then most probably the user forgot to add `library`
      // before the library name.
      CARBON_DIAGNOSTIC(MissingLibraryKeyword, Error,
                        "Missing `library` keyword.");
      context.emitter().Emit(*context.position(), MissingLibraryKeyword);
      on_parse_error();
      return;
    }
  }

  std::optional<Tree::ApiOrImpl> api_or_impl;
  if (directive != NodeKind::ImportDirective) {
    api_or_impl = HandleApiOrImpl(context);
    if (!api_or_impl) {
      on_parse_error();
      return;
    }
  }

  if (auto semi = context.ConsumeIf(Lex::TokenKind::Semi)) {
    if (directive == NodeKind::ImportDirective) {
      context.AddImport(names);
    } else {
      context.set_packaging_directive(names, *api_or_impl);
    }

    context.AddNode(directive, *semi, state.subtree_start, state.has_error);
  } else {
    context.DiagnoseExpectedDeclSemi(context.tokens().GetKind(state.token));
    on_parse_error();
  }
}

// Returns true if currently in a valid state for imports, false otherwise. May
// update the packaging state respectively.
static auto VerifyInImports(Context& context, Lex::TokenIndex intro_token)
    -> bool {
  switch (context.packaging_state()) {
    case Context::PackagingState::FileStart:
      // `package` is no longer allowed, but `import` may repeat.
      context.set_packaging_state(Context::PackagingState::InImports);
      return true;

    case Context::PackagingState::InImports:
      return true;

    case Context::PackagingState::AfterNonPackagingDecl: {
      context.set_packaging_state(
          Context::PackagingState::InImportsAfterNonPackagingDecl);
      CARBON_DIAGNOSTIC(ImportTooLate, Error,
                        "`import` directives must come after the `package` "
                        "directive (if present) and before any other "
                        "entities in the file.");
      CARBON_DIAGNOSTIC(FirstDecl, Note, "First declaration is here.");
      context.emitter()
          .Build(intro_token, ImportTooLate)
          .Note(context.first_non_packaging_token(), FirstDecl)
          .Emit();
      return false;
    }

    case Context::PackagingState::InImportsAfterNonPackagingDecl:
      // There is a sequential block of misplaced `import` statements, which can
      // occur if a declaration is added above `import`s. Avoid duplicate
      // warnings.
      return false;
  }
}

// Common logic for both `import` and `export import`, distinguished by whether
// `export_token` is valid.
static auto HandleImportHelper(Context& context,
                               const Context::StateStackEntry& state,
                               Lex::TokenIndex export_token) -> void {
  auto directive = NodeKind::ImportDirective;
  auto on_parse_error = [&] { OnParseError(context, state, directive); };

  auto intro_token = context.ConsumeChecked(Lex::TokenKind::Import);
  context.AddLeafNode(NodeKind::ImportIntroducer, intro_token);

  if (export_token.is_valid()) {
    context.AddLeafNode(NodeKind::ImportExport, export_token);
  }

  if (VerifyInImports(context, intro_token)) {
    HandleDirectiveContent(context, state, directive, export_token.is_valid(),
                           on_parse_error);
  } else {
    on_parse_error();
  }
}

auto HandleImportAsRegular(Context& context) -> void {
  auto state = context.PopState();

  HandleImportHelper(context, state, /*export_token=*/Lex::TokenIndex::Invalid);
}

// Diagnoses if `export` is used in an `impl` file.
static auto RestrictExportToApi(Context& context,
                                Context::StateStackEntry& state) -> void {
  // Error for both Main//default and every implementation file.
  auto packaging = context.tree().packaging_directive();
  if (!packaging || packaging->api_or_impl == Tree::ApiOrImpl::Impl) {
    CARBON_DIAGNOSTIC(ExportFromImpl, Error,
                      "`export` is only allowed in API files.");
    context.emitter().Emit(state.token, ExportFromImpl);
    state.has_error = true;
  }
}

auto HandleImportAsExport(Context& context) -> void {
  auto state = context.PopState();

  context.ConsumeChecked(Lex::TokenKind::Export);
  RestrictExportToApi(context, state);

  HandleImportHelper(context, state, state.token);
}

auto HandleExportName(Context& context) -> void {
  auto state = context.PopState();

  RestrictExportToApi(context, state);

  context.PushState(state, State::ExportNameFinish);
  context.PushState(State::DeclNameAndParamsAsNone, state.token);
}

auto HandleExportNameFinish(Context& context) -> void {
  auto state = context.PopState();

  context.AddNodeExpectingDeclSemi(state, NodeKind::ExportDirective,
                                   Lex::TokenKind::Export,
                                   /*is_def_allowed=*/false);
}

// Handles common logic for `package` and `library`.
static auto HandlePackageAndLibraryDirectives(Context& context,
                                              Lex::TokenKind intro_token_kind,
                                              NodeKind intro,
                                              NodeKind directive) -> void {
  auto state = context.PopState();

  auto on_parse_error = [&] { OnParseError(context, state, directive); };

  auto intro_token = context.ConsumeChecked(intro_token_kind);
  context.AddLeafNode(intro, intro_token);

  if (intro_token != Lex::TokenIndex::FirstNonCommentToken) {
    CARBON_DIAGNOSTIC(PackageTooLate, Error,
                      "The `{0}` directive must be the first non-comment line.",
                      Lex::TokenKind);
    CARBON_DIAGNOSTIC(FirstNonCommentLine, Note,
                      "First non-comment line is here.");
    context.emitter()
        .Build(intro_token, PackageTooLate, intro_token_kind)
        .Note(Lex::TokenIndex::FirstNonCommentToken, FirstNonCommentLine)
        .Emit();
    on_parse_error();
    return;
  }

  // `package`/`library` is no longer allowed, but `import` may repeat.
  context.set_packaging_state(Context::PackagingState::InImports);

  HandleDirectiveContent(context, state, directive, /*is_export=*/false,
                         on_parse_error);
}

auto HandlePackage(Context& context) -> void {
  HandlePackageAndLibraryDirectives(context, Lex::TokenKind::Package,
                                    NodeKind::PackageIntroducer,
                                    NodeKind::PackageDirective);
}

auto HandleLibrary(Context& context) -> void {
  HandlePackageAndLibraryDirectives(context, Lex::TokenKind::Library,
                                    NodeKind::LibraryIntroducer,
                                    NodeKind::LibraryDirective);
}

}  // namespace Carbon::Parse
