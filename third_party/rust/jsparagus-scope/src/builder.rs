//! Data collection.
//!
//! `ScopeBuilder` is the entry point of this module.
//!
//! Each AST node that will hold bindings (global, block, function, etc.) has a
//! corresponding scope builder type defined in this module:
//!   * `GlobalScopeBuilder`
//!   * `BlockScopeBuilder`
//!   * `FunctionExpressionScopeBuilder`
//!   * `FunctionParametersScopeBuilder`
//!   * `FunctionBodyScopeBuilder`
//!
//! They follow the pattern:
//!   * They are created and pushed to the `ScopeBuilderStack` when the
//!     algorithm enters a scope.
//!   * They collect the information necessary to build a `ScopeData` object
//!     (which will eventually become a `js::ScopeCreationData` on the
//!      C++ side).
//!   * They stay on the scope builder stack until the algorithm leaves that
//!     scope.
//!   * Then they are converted by `into_scope_data()`.
//!
//! Fields in the builder types mostly correspond to local variables in spec
//! algorithms.  For example, `GlobalScopeBuilder` has fields named
//! `functions_to_initialize`, `declared_function_names`, and
//! `declared_var_names` which correspond to the
//! [GlobalDeclarationInstantiation][1] algorithm's local variables
//! *functionsToInitialize*, *declaredFunctionNames*, and *declaredVarNames*.
//!
//! This module performs some steps of those algorithms -- the parts that can
//! be done at compile time. The results are passed along to the emitter and
//! ultimately the JS runtime.
//!
//! [1]: https://tc39.es/ecma262/#sec-globaldeclarationinstantiation

use crate::data::FunctionDeclarationPropertyMap;
use crate::free_name_tracker::FreeNameTracker;
use ast::associated_data::AssociatedData;
use ast::source_atom_set::{CommonSourceAtomSetIndices, SourceAtomSetIndex};
use ast::source_location_accessor::SourceLocationAccessor;
use ast::type_id::NodeTypeIdAccessor;
use indexmap::set::IndexSet;
use std::collections::hash_map::Keys;
use std::collections::{HashMap, HashSet};
use stencil::function::{FunctionFlags, FunctionSyntaxKind};
use stencil::scope::{
    BindingName, FunctionScopeData, GlobalScopeData, LexicalScopeData, ScopeData, ScopeDataList,
    ScopeDataMap, ScopeIndex, VarScopeData,
};
use stencil::script::{ScriptStencil, ScriptStencilIndex, ScriptStencilList, SourceExtent};

/// The kind of items inside the result of VarScopedDeclarations.
///
/// This enum isn't actually used, but just for simplifying comment in
/// ScopeKind.
#[derive(Debug, Clone, PartialEq)]
enum VarScopedDeclarationsItemKind {
    /// Static Semantics: VarScopedDeclarations
    /// https://tc39.es/ecma262/#sec-variable-statement-static-semantics-varscopeddeclarations
    ///
    /// VariableDeclarationList : VariableDeclaration
    ///
    /// 1. Return a new List containing VariableDeclaration.
    ///
    /// VariableDeclarationList : VariableDeclarationList, VariableDeclaration
    ///
    /// 1. Let declarations be VarScopedDeclarations of VariableDeclarationList.
    /// 2. Append VariableDeclaration to declarations.
    /// 3. Return declarations.
    #[allow(dead_code)]
    VariableDeclaration,

    /// Static Semantics: VarScopedDeclarations
    /// https://tc39.es/ecma262/#sec-for-in-and-for-of-statements-static-semantics-varscopeddeclarations
    ///
    /// IterationStatement :
    ///   for ( var ForBinding in Expression ) Statement
    ///   for ( var ForBinding of AssignmentExpression ) Statement
    ///   for await ( var ForBinding of AssignmentExpression ) Statement
    ///
    /// 1. Let declarations be a List containing ForBinding.
    /// 2. Append to declarations the elements of the VarScopedDeclarations of
    ///    Statement.
    /// 3. Return declarations.
    #[allow(dead_code)]
    ForBinding,

    /// Static Semantics: VarScopedDeclarations
    /// https://tc39.es/ecma262/#sec-function-definitions-static-semantics-varscopeddeclarations
    ///
    /// FunctionStatementList : StatementList
    ///
    /// 1. Return the TopLevelVarScopedDeclarations of StatementList.

    /// Static Semantics: VarScopedDeclarations
    /// https://tc39.es/ecma262/#sec-scripts-static-semantics-varscopeddeclarations
    ///
    /// ScriptBody : StatementList
    ///
    /// 1. Return TopLevelVarScopedDeclarations of StatementList.

    /// Static Semantics: TopLevelVarScopedDeclarations
    /// https://tc39.es/ecma262/#sec-block-static-semantics-toplevelvarscopeddeclarations
    ///
    /// StatementListItem : Declaration
    ///
    /// 1. If Declaration is Declaration : HoistableDeclaration, then
    ///   a. Let declaration be DeclarationPart of HoistableDeclaration.
    ///   b. Return « declaration ».
    /// 2. Return a new empty List.

    /// Static Semantics: DeclarationPart
    /// https://tc39.es/ecma262/#sec-static-semantics-declarationpart
    ///
    /// HoistableDeclaration : FunctionDeclaration
    ///
    /// 1. Return FunctionDeclaration.
    #[allow(dead_code)]
    FunctionDeclaration,

    /// HoistableDeclaration : GeneratorDeclaration
    ///
    /// 1. Return GeneratorDeclaration.
    #[allow(dead_code)]
    GeneratorDeclaration,

    /// HoistableDeclaration : AsyncFunctionDeclaration
    ///
    /// 1. Return AsyncFunctionDeclaration.
    #[allow(dead_code)]
    AsyncFunctionDeclaration,

    /// HoistableDeclaration : AsyncGeneratorDeclaration
    ///
    /// 1. Return AsyncGeneratorDeclaration.
    #[allow(dead_code)]
    AsyncGeneratorDeclaration,

    /// Static Semantics: TopLevelVarScopedDeclarations
    /// https://tc39.es/ecma262/#sec-labelled-statements-static-semantics-toplevelvarscopeddeclarations
    ///
    /// LabelledItem : FunctionDeclaration
    ///
    /// 1. Return a new List containing FunctionDeclaration.
    /* FunctionDeclaration */

    /// Annex B Initializers in ForIn Statement Heads
    /// https://tc39.es/ecma262/#sec-initializers-in-forin-statement-heads
    ///
    /// IterationStatement :
    ///   for ( var BindingIdentifier Initializer in Expression ) Statement
    ///
    /// 1. Let declarations be a List containing BindingIdentifier.
    /// 2. Append to declarations the elements of the VarScopedDeclarations of
    ///    Statement.
    /// 3. Return declarations.
    #[allow(dead_code)]
    BindingIdentifier,
}

/// The kind of items inside the result of LexicallyScopedDeclarations.
///
/// This enum isn't actually used, but just for simplifying comment in
/// ScopeKind.
#[derive(Debug, Clone, PartialEq)]
enum LexicallyScopedDeclarations {
    /// Static Semantics: LexicallyScopedDeclarations
    /// https://tc39.es/ecma262/#sec-block-static-semantics-lexicallyscopeddeclarations
    ///
    /// StatementListItem : Declaration
    ///
    /// 1. Return a new List containing DeclarationPart of Declaration.

    /// Static Semantics: DeclarationPart
    /// https://tc39.es/ecma262/#sec-static-semantics-declarationpart
    ///
    /// HoistableDeclaration : FunctionDeclaration
    ///
    /// 1. Return FunctionDeclaration.
    #[allow(dead_code)]
    FunctionDeclaration,

    /// HoistableDeclaration : GeneratorDeclaration
    ///
    /// 1. Return GeneratorDeclaration.
    #[allow(dead_code)]
    GeneratorDeclaration,

    /// HoistableDeclaration : AsyncFunctionDeclaration
    ///
    /// 1. Return AsyncFunctionDeclaration.
    #[allow(dead_code)]
    AsyncFunctionDeclaration,

    /// HoistableDeclaration : AsyncGeneratorDeclaration
    ///
    /// 1. Return AsyncGeneratorDeclaration.
    #[allow(dead_code)]
    AsyncGeneratorDeclaration,

    /// Declaration : ClassDeclaration
    ///
    /// 1. Return ClassDeclaration.
    #[allow(dead_code)]
    ClassDeclaration,

    /// Declaration : LexicalDeclaration
    ///
    /// 1. Return LexicalDeclaration.
    #[allow(dead_code)]
    LexicalDeclarationWithLet,
    #[allow(dead_code)]
    LexicalDeclarationWithConst,

    /// Static Semantics: LexicallyScopedDeclarations
    /// https://tc39.es/ecma262/#sec-labelled-statements-static-semantics-lexicallyscopeddeclarations
    ///
    /// LabelledItem : FunctionDeclaration
    ///
    /// 1. Return a new List containing FunctionDeclaration.
    /* FunctionDeclaration */

    /// Static Semantics: LexicallyScopedDeclarations
    /// https://tc39.es/ecma262/#sec-function-definitions-static-semantics-lexicallyscopeddeclarations
    ///
    /// FunctionStatementList : StatementList
    ///
    /// 1. Return the TopLevelLexicallyScopedDeclarations of StatementList.

    /// Static Semantics: LexicallyScopedDeclarations
    /// https://tc39.es/ecma262/#sec-scripts-static-semantics-lexicallyscopeddeclarations
    ///
    /// ScriptBody : StatementList
    ///
    /// 1. Return TopLevelLexicallyScopedDeclarations of StatementList.

    /// Static Semantics: TopLevelLexicallyScopedDeclarations
    /// https://tc39.es/ecma262/#sec-block-static-semantics-toplevellexicallyscopeddeclarations
    ///
    /// StatementListItem : Declaration
    ///
    /// 1. If Declaration is Declaration : HoistableDeclaration, then
    ///   a. Return « ».
    /// 2. Return a new List containing Declaration.

    /// Static Semantics: LexicallyScopedDeclarations
    /// https://tc39.es/ecma262/#sec-exports-static-semantics-lexicallyscopeddeclarations
    ///
    /// ExportDeclaration : export Declaration
    ///
    /// 1. Return a new List containing DeclarationPart of Declaration.

    /// ExportDeclaration : export default HoistableDeclaration
    ///
    /// 1. Return a new List containing DeclarationPart of HoistableDeclaration.

    /// ExportDeclaration : export default ClassDeclaration
    ///
    /// 1. Return a new List containing ClassDeclaration.
    /* ClassDeclaration */

    /// ExportDeclaration : export default AssignmentExpression ;
    ///
    /// 1. Return a new List containing this ExportDeclaration.
    #[allow(dead_code)]
    ExportDeclarationWithAssignmentExpression,
}

/// Items on the ScopeBuilder.scope_stack.
/// Specifies the kind of BindingIdentifier.
///
/// This includes only BindingIdentifier that appears inside list or recursive
/// structure.
///
/// BindingIdentifier that appears only once for a structure
/// (e.g. Function.name) should be handled immediately, without using
/// ScopeBuilder.scope_stack.
#[derive(Debug, Clone, PartialEq)]
enum ScopeKind {
    /// VarScopedDeclarationsItemKind::VariableDeclaration
    /// VarScopedDeclarationsItemKind::ForBinding
    /// VarScopedDeclarationsItemKind::BindingIdentifier
    Var,

    /// LexicallyScopedDeclarations::LexicalDeclarationWithLet
    Let,

    /// LexicallyScopedDeclarations::LexicalDeclarationWithConst
    Const,

    FunctionName,

    FormalParameter,

    #[allow(dead_code)]
    CatchParameter,

    /// LexicallyScopedDeclarations::ExportDeclarationWithAssignmentExpression
    #[allow(dead_code)]
    Export,

    /// VarScopedDeclarationsItemKind::FunctionDeclaration
    /// VarScopedDeclarationsItemKind::GeneratorDeclaration
    /// VarScopedDeclarationsItemKind::AsyncFunctionDeclaration
    /// VarScopedDeclarationsItemKind::AsyncGeneratorDeclaration
    #[allow(dead_code)]
    ScriptBodyStatementList,
    #[allow(dead_code)]
    FunctionStatementList,

    /// LexicallyScopedDeclarations::FunctionDeclaration
    /// LexicallyScopedDeclarations::GeneratorDeclaration
    /// LexicallyScopedDeclarations::AsyncFunctionDeclaration
    /// LexicallyScopedDeclarations::AsyncGeneratorDeclaration
    /// LexicallyScopedDeclarations::ClassDeclaration
    #[allow(dead_code)]
    BlockStatementList,
}

/// Index into BaseScopeData.bindings.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BindingIndex {
    index: usize,
}
impl BindingIndex {
    fn new(index: usize) -> Self {
        Self { index }
    }

    pub fn next(&self) -> Self {
        Self {
            index: self.index + 1,
        }
    }
}

impl From<BindingIndex> for usize {
    fn from(index: BindingIndex) -> usize {
        index.index
    }
}

#[derive(Debug)]
struct PossiblyAnnexBFunction {
    name: SourceAtomSetIndex,
    owner_scope_index: ScopeIndex,
    binding_index: BindingIndex,

    /// Index of the script in the list of `functions` in the
    /// `FunctionScriptStencilBuilder`.
    script_index: ScriptStencilIndex,
}

#[derive(Debug)]
struct PossiblyAnnexBFunctionList {
    functions: HashMap<SourceAtomSetIndex, Vec<PossiblyAnnexBFunction>>,
}

impl PossiblyAnnexBFunctionList {
    fn new() -> Self {
        Self {
            functions: HashMap::new(),
        }
    }

    fn push(
        &mut self,
        name: SourceAtomSetIndex,
        owner_scope_index: ScopeIndex,
        binding_index: BindingIndex,
        script_index: ScriptStencilIndex,
    ) {
        if let Some(functions) = self.functions.get_mut(&name) {
            functions.push(PossiblyAnnexBFunction {
                name,
                owner_scope_index,
                binding_index,
                script_index,
            });
            return;
        }

        let mut functions = Vec::with_capacity(1);
        functions.push(PossiblyAnnexBFunction {
            name,
            owner_scope_index,
            binding_index,
            script_index,
        });
        self.functions.insert(name, functions);
    }

    fn remove_if_exists(&mut self, name: SourceAtomSetIndex) {
        self.functions.remove(&name);
    }

    fn mark_annex_b(
        &self,
        scopes: &mut ScopeDataList,
        function_declaration_properties: &mut FunctionDeclarationPropertyMap,
    ) {
        for functions in &mut self.functions.values() {
            for fun in functions {
                let scope = scopes.get_mut(fun.owner_scope_index);
                match scope {
                    ScopeData::Lexical(data) => {
                        data.mark_annex_b_function(fun.name, fun.binding_index.into());
                    }
                    _ => panic!("unexpected scope pointed by Annex B function"),
                }

                function_declaration_properties.mark_annex_b(fun.script_index);
            }
        }
    }

    fn names(&self) -> Keys<SourceAtomSetIndex, Vec<PossiblyAnnexBFunction>> {
        self.functions.keys()
    }

    fn clear(&mut self) {
        self.functions.clear();
    }
}

/// Common fields across all *ScopeBuilder.
#[derive(Debug)]
struct BaseScopeBuilder {
    name_tracker: FreeNameTracker,

    /// Bindings in this scope can be accessed dynamically by:
    ///   * direct `eval`
    ///   * `with` statement
    ///   * `delete name` statement
    bindings_accessed_dynamically: bool,
}

impl BaseScopeBuilder {
    fn new() -> Self {
        Self {
            name_tracker: FreeNameTracker::new(),
            bindings_accessed_dynamically: false,
        }
    }
}

/// Variables declared/used in GlobalDeclarationInstantiation.
#[derive(Debug)]
struct GlobalScopeBuilder {
    base: BaseScopeBuilder,

    /// Runtime Semantics: GlobalDeclarationInstantiation ( script, env )
    /// https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
    ///
    /// Step 8. Let functionsToInitialize be a new empty List.
    functions_to_initialize: Vec<ScriptStencilIndex>,

    /// Step 9. Let declaredFunctionNames be a new empty List.
    declared_function_names: IndexSet<SourceAtomSetIndex>,

    /// Step 11. Let declaredVarNames be a new empty List.
    /// NOTE: This is slightly different than the spec that this can contain
    ///       names in declaredFunctionNames.
    ///       The duplication should be filtered before the use.
    declared_var_names: IndexSet<SourceAtomSetIndex>,

    /// Step 15. Let lexDeclarations be the LexicallyScopedDeclarations of
    ///          script.
    let_names: Vec<SourceAtomSetIndex>,
    const_names: Vec<SourceAtomSetIndex>,

    scope_index: ScopeIndex,
}

impl GlobalScopeBuilder {
    fn new(scope_index: ScopeIndex) -> Self {
        Self {
            base: BaseScopeBuilder::new(),
            functions_to_initialize: Vec::new(),
            declared_function_names: IndexSet::new(),
            declared_var_names: IndexSet::new(),
            let_names: Vec::new(),
            const_names: Vec::new(),
            scope_index,
        }
    }

    fn declare_var(&mut self, name: SourceAtomSetIndex) {
        // Runtime Semantics: GlobalDeclarationInstantiation ( script, env )
        // https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
        //
        // Step 7. Let varDeclarations be the VarScopedDeclarations of script.
        //
        // Step 12. For each d in varDeclarations, do
        // Step 12.a. If d is a VariableDeclaration, a ForBinding, or a
        //            BindingIdentifier, then
        // Step 12.a.i. For each String vn in the BoundNames of d, do
        // (implicit)

        // Step 12.a.i.i If vn is not an element of declaredFunctionNames, then
        // (done in remove_function_names_from_var_names)

        // Step 12.a.i.1.a. Let vnDefinable be ? envRec.CanDeclareGlobalVar(vn).
        // Step 12.a.i.1.b. If vnDefinable is false, throw a TypeError
        //                  exception.
        // (done in runtime)

        // Step 12.a.i.1.c. If vn is not an element of declaredVarNames, then
        // Step 12.a.i.1.a.i. Append vn to declaredVarNames.
        self.declared_var_names.insert(name);
    }

    fn declare_let(&mut self, name: SourceAtomSetIndex) {
        // Runtime Semantics: GlobalDeclarationInstantiation ( script, env )
        // https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
        //
        // Step 15. Let lexDeclarations be the LexicallyScopedDeclarations of
        //          script.
        self.let_names.push(name);
    }

    fn declare_const(&mut self, name: SourceAtomSetIndex) {
        // Runtime Semantics: GlobalDeclarationInstantiation ( script, env )
        // https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
        //
        // Step 15. Let lexDeclarations be the LexicallyScopedDeclarations of
        //          script.
        self.const_names.push(name);
    }

    fn declare_function(&mut self, name: SourceAtomSetIndex, fun_index: ScriptStencilIndex) {
        // Runtime Semantics: GlobalDeclarationInstantiation ( script, env )
        // https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
        //
        // Step 10. For each d in varDeclarations, in reverse list order, do
        // Step 10.a. If d is neither a VariableDeclaration nor a ForBinding
        //            nor a BindingIdentifier, then
        // (implicit)

        // Step 10.a.i. Assert: d is either a FunctionDeclaration,
        //              a GeneratorDeclaration, an AsyncFunctionDeclaration,
        //              or an AsyncGeneratorDeclaration.

        // Step 10.a.ii. NOTE: If there are multiple function declarations for
        //               the same name, the last declaration is used.

        // Step 10.a.iii. Let fn be the sole element of the BoundNames of d.

        // Step 10.a.iv. If fn is not an element of declaredFunctionNames, then
        //
        // NOTE: Instead of iterating in reverse list oder, we iterate in
        // normal order and overwrite existing item.

        // Steps 10.a.iv.1. Let fnDefinable be
        //                  ? envRec.CanDeclareGlobalFunction(fn).
        // Steps 10.a.iv.2. If fnDefinable is false, throw a TypeError
        //                  exception.
        // (done in runtime)

        // Step 10.a.iv.3. Append fn to declaredFunctionNames.
        self.declared_function_names.insert(name);

        // Step 10.a.iv.4. Insert d as the first element of
        //                 functionsToInitialize.
        self.functions_to_initialize.push(fun_index);
    }

    fn remove_function_names_from_var_names(&mut self) {
        // Runtime Semantics: GlobalDeclarationInstantiation ( script, env )
        // https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
        //
        // Step 12.a.i.i If vn is not an element of declaredFunctionNames, then
        //
        // To avoid doing 2-pass, we note all var names, and filter function
        // names out after visiting all of them.
        for n in &self.declared_function_names {
            self.declared_var_names.remove(n);
        }
    }

    fn perform_annex_b(
        &mut self,
        scopes: &mut ScopeDataList,
        function_declaration_properties: &mut FunctionDeclarationPropertyMap,
        possibly_annex_b_functions: &mut PossiblyAnnexBFunctionList,
    ) {
        // Annex B
        // Changes to GlobalDeclarationInstantiation
        // https://tc39.es/ecma262/#sec-web-compat-globaldeclarationinstantiation
        //
        // Step 1. Let strict be IsStrict of script.
        //
        // FIXME: Once directives are supported, reflect it here.
        let strict = false;

        // Step 2. If strict is false, then
        if strict {
            return;
        }

        // Step 2.a. Let declaredFunctionOrVarNames be a new empty List.
        // Step 2.b. Append to declaredFunctionOrVarNames the elements of
        //           declaredFunctionNames.
        // Step 2.c. Append to declaredFunctionOrVarNames the elements of
        //           declaredVarNames.
        //
        // NOTE: Use `self.declared_var_names` to avoid duplication against
        //       `declaredVarNames`.
        //       And duplication against `declaredFunctionNames` will be
        //       removed in `remove_function_names_from_var_names`.

        // Step 2.d. For each FunctionDeclaration f that is directly contained
        //           in the StatementList of a Block, CaseClause, or
        //           DefaultClause Contained within script, do
        //
        // NOTE: `possibly_annex_b_functions` contains all of them.

        // Step 2.d.i. Let F be StringValue of the BindingIdentifier of f.
        // Step 2.d.ii. If replacing the FunctionDeclaration f with a
        //              VariableStatement that has F as a BindingIdentifier
        //              would not produce any Early Errors for script, then
        //
        // NOTE: Early Errors happen if any of top-level lexical has
        //       the same name.  Filter out those functions here.
        for n in &self.let_names {
            possibly_annex_b_functions.remove_if_exists(*n);
        }
        for n in &self.const_names {
            possibly_annex_b_functions.remove_if_exists(*n);
        }

        // Step 2.d.ii.1. If env.HasLexicalDeclaration(F) is false, then
        // Step 2.d.ii.1.a. Let fnDefinable be ? env.CanDeclareGlobalVar(F).
        // Step 2.d.ii.1.b. If fnDefinable is true, then
        //
        // FIXME: Are these steps performed by any implementation?
        //        https://github.com/tc39/ecma262/issues/2019

        // Step 2.d.ii.1.b.i. NOTE: A var binding for F is only instantiated
        //                    here if it is neither a VarDeclaredName nor
        //                    the name of another FunctionDeclaration.
        // Step 2.d.ii.1.b.ii. If declaredFunctionOrVarNames does not
        //                     contain F, then
        // Step 2.d.ii.1.b.ii.1. Perform
        //                       ?env.CreateGlobalVarBinding(F, false).
        // Step 2.d.ii.1.b.ii.2. Append F to declaredFunctionOrVarNames.
        for n in possibly_annex_b_functions.names() {
            self.declared_var_names.insert(*n);
        }

        // Step 2.d.ii.1.b.iii. When the FunctionDeclaration f is evaluated,
        //                      perform the following steps in place of the
        //                      FunctionDeclaration Evaluation algorithm
        //                      provided in
        //                      https://tc39.es/ecma262/#sec-function-definitions-runtime-semantics-evaluation :
        // Step 2.d.ii.1.b.iii.1. Let genv be the running execution
        //                        context's VariableEnvironment.
        // Step 2.d.ii.1.b.iii.2. Let benv be the running execution
        //                        context's LexicalEnvironment.
        // Step 2.d.ii.1.b.iii.3. Let fobj be
        //                        ! benv.GetBindingValue(F, false).
        // Step 2.d.ii.1.b.iii.4. Perform
        //                        ? genv.SetMutableBinding(F, fobj, false).
        // Step 2.d.ii.1.b.iii.5. Return NormalCompletion(empty).
        possibly_annex_b_functions.mark_annex_b(scopes, function_declaration_properties);
    }

    fn into_scope_data(
        mut self,
        scopes: &mut ScopeDataList,
        function_declaration_properties: &mut FunctionDeclarationPropertyMap,
        possibly_annex_b_functions: &mut PossiblyAnnexBFunctionList,
    ) -> ScopeData {
        // Runtime Semantics: GlobalDeclarationInstantiation ( script, env )
        // https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
        //
        // NOTE: Steps are reordered to match the order of binding in runtime.

        // Step 13. NOTE: Annex B adds additional steps at this point.
        //
        // NOTE: Reordered here to reflect the change to
        //       self.declared_var_names.
        self.perform_annex_b(
            scopes,
            function_declaration_properties,
            possibly_annex_b_functions,
        );

        // Step 12.a.i.i If vn is not an element of declaredFunctionNames, then
        self.remove_function_names_from_var_names();

        let mut data = GlobalScopeData::new(
            self.declared_var_names.len() + self.declared_function_names.len(),
            self.let_names.len(),
            self.const_names.len(),
            self.functions_to_initialize,
        );

        // Step 18. For each String vn in declaredVarNames, in list order, do
        for n in &self.declared_var_names {
            // 18.a. Perform ? envRec.CreateGlobalVarBinding(vn, false).
            let is_closed_over = self.base.name_tracker.is_closed_over_def(n);
            data.base
                .bindings
                .push(BindingName::new(*n, is_closed_over))
        }

        // Step 17. For each Parse Node f in functionsToInitialize, do
        for n in &self.declared_function_names {
            // Step 17.a. Let fn be the sole element of the BoundNames of f.
            // Step 17.b. Let fo be InstantiateFunctionObject of f with
            //            argument env.
            // Step 17.c. Perform
            //            ? envRec.CreateGlobalFunctionBinding(fn, fo, false).
            let is_closed_over = self.base.name_tracker.is_closed_over_def(n);
            data.base
                .bindings
                .push(BindingName::new_top_level_function(*n, is_closed_over));
        }

        // Step 15. Let lexDeclarations be the LexicallyScopedDeclarations of
        //          script.
        // Step 16. For each element d in lexDeclarations, do
        // Step 16.b. For each element dn of the BoundNames of d, do
        for n in &self.let_names {
            // Step 16.b.ii. Else,
            // Step 16.b.ii.1. Perform ? envRec.CreateMutableBinding(dn, false).
            let is_closed_over = self.base.name_tracker.is_closed_over_def(n);
            data.base
                .bindings
                .push(BindingName::new(*n, is_closed_over))
        }
        for n in &self.const_names {
            // Step 16.b.i. If IsConstantDeclaration of d is true, then
            // Step 16.b.i.1. Perform ? envRec.CreateImmutableBinding(dn, true).
            let is_closed_over = self.base.name_tracker.is_closed_over_def(n);
            data.base
                .bindings
                .push(BindingName::new(*n, is_closed_over))
        }

        ScopeData::Global(data)
    }
}

#[derive(Debug)]
struct FunctionNameAndStencilIndex {
    name: SourceAtomSetIndex,
    stencil: ScriptStencilIndex,
}

/// Variables declared/used in BlockDeclarationInstantiation
#[derive(Debug)]
struct BlockScopeBuilder {
    base: BaseScopeBuilder,

    /// Runtime Semantics: BlockDeclarationInstantiation ( code, env )
    /// https://tc39.es/ecma262/#sec-blockdeclarationinstantiation
    ///
    /// Step 3. Let declarations be the LexicallyScopedDeclarations of code.
    let_names: Vec<SourceAtomSetIndex>,
    const_names: Vec<SourceAtomSetIndex>,

    /// Runtime Semantics: BlockDeclarationInstantiation ( code, env )
    /// https://tc39.es/ecma262/#sec-blockdeclarationinstantiation
    ///
    /// Step 4.b. If d is a FunctionDeclaration, a GeneratorDeclaration, an
    ///           AsyncFunctionDeclaration, or an AsyncGeneratorDeclaration,
    ///           then
    functions: Vec<FunctionNameAndStencilIndex>,

    /// Scope associated to this builder.
    scope_index: ScopeIndex,
}

impl BlockScopeBuilder {
    fn new(scope_index: ScopeIndex) -> Self {
        Self {
            base: BaseScopeBuilder::new(),
            let_names: Vec::new(),
            const_names: Vec::new(),
            functions: Vec::new(),
            scope_index,
        }
    }

    fn declare_let(&mut self, name: SourceAtomSetIndex) {
        // Runtime Semantics: BlockDeclarationInstantiation ( code, env )
        // https://tc39.es/ecma262/#sec-blockdeclarationinstantiation
        //
        // Step 3. Let declarations be the LexicallyScopedDeclarations of code.
        self.let_names.push(name);
    }

    fn declare_const(&mut self, name: SourceAtomSetIndex) {
        // Runtime Semantics: BlockDeclarationInstantiation ( code, env )
        // https://tc39.es/ecma262/#sec-blockdeclarationinstantiation
        //
        // Step 3. Let declarations be the LexicallyScopedDeclarations of code.
        self.const_names.push(name);
    }

    fn declare_function(&mut self, name: SourceAtomSetIndex, fun_index: ScriptStencilIndex) {
        // Runtime Semantics: BlockDeclarationInstantiation ( code, env )
        // https://tc39.es/ecma262/#sec-blockdeclarationinstantiation
        //
        // Step 3. Let declarations be the LexicallyScopedDeclarations of code.
        //
        // Step 4.b. If d is a FunctionDeclaration, a GeneratorDeclaration, an
        //           AsyncFunctionDeclaration, or an AsyncGeneratorDeclaration,
        //           then
        self.functions.push(FunctionNameAndStencilIndex {
            name,
            stencil: fun_index,
        });
    }

    fn into_scope_data(
        self,
        enclosing: ScopeIndex,
        possibly_annex_b_functions: &mut PossiblyAnnexBFunctionList,
    ) -> ScopeData {
        let mut data = LexicalScopeData::new_block(
            self.let_names.len() + self.functions.len(),
            self.const_names.len(),
            enclosing,
            self.functions.iter().map(|n| n.stencil).collect(),
        );

        // Runtime Semantics: BlockDeclarationInstantiation ( code, env )
        // https://tc39.es/ecma262/#sec-blockdeclarationinstantiation
        //
        // Step 1. Let envRec be env's EnvironmentRecord.
        // Step 2. Assert: envRec is a declarative Environment Record.
        // (implicit)

        // Step 4. For each element d in declarations, do
        // Step 4.a. For each element dn of the BoundNames of d, do
        for n in &self.let_names {
            // Step 4.a.ii. Else,
            // Step 4.a.ii.1. Perform ! envRec.CreateMutableBinding(dn, false).
            let is_closed_over = self.base.name_tracker.is_closed_over_def(n);
            data.base
                .bindings
                .push(BindingName::new(*n, is_closed_over));
        }
        for n in &self.functions {
            // Step 4.b. If d is a FunctionDeclaration, a GeneratorDeclaration,
            //           an AsyncFunctionDeclaration,
            //           or an AsyncGeneratorDeclaration, then
            // Step 4.b.i. Let fn be the sole element of the BoundNames of d.
            // Step 4.b.ii. Let fo be InstantiateFunctionObject of d with
            //              argument env.
            // Step 4.b.iii. Perform envRec.InitializeBinding(fn, fo).
            let is_closed_over = self.base.name_tracker.is_closed_over_def(&n.name);
            let binding_index = BindingIndex::new(data.base.bindings.len());
            data.base
                .bindings
                .push(BindingName::new(n.name, is_closed_over));

            possibly_annex_b_functions.push(n.name, self.scope_index, binding_index, n.stencil);
        }
        for n in &self.const_names {
            // Step 4.a.i. If IsConstantDeclaration of d is true, then
            // Step 4.a.i.1. Perform ! envRec.CreateImmutableBinding(dn, true).
            let is_closed_over = self.base.name_tracker.is_closed_over_def(n);
            data.base
                .bindings
                .push(BindingName::new(*n, is_closed_over));
        }

        ScopeData::Lexical(data)
    }
}

/// Scope for a FunctionExpression.
///
/// The FunctionExpression `(function f() { return f; })` introduces a lexical
/// scope with a single binding `f`, set to the function itself. We create this
/// scope builder whether the FunctionExpression has a name or not, for
/// consistency.
#[derive(Debug)]
struct FunctionExpressionScopeBuilder {
    base: BaseScopeBuilder,

    function_expression_name: Option<SourceAtomSetIndex>,

    scope_index: ScopeIndex,
}

impl FunctionExpressionScopeBuilder {
    fn new(scope_index: ScopeIndex) -> Self {
        Self {
            base: BaseScopeBuilder::new(),
            function_expression_name: None,
            scope_index,
        }
    }

    fn set_function_name(&mut self, name: SourceAtomSetIndex) {
        self.function_expression_name = Some(name);
    }

    fn into_scope_data(self, enclosing: ScopeIndex) -> ScopeData {
        match &self.function_expression_name {
            Some(name) => {
                // Runtime Semantics: Evaluation
                // https://tc39.es/ecma262/#sec-function-definitions-runtime-semantics-evaluation
                //
                // FunctionExpression :
                //   function BindingIdentifier ( FormalParameters )
                //   { FunctionBody }
                //
                // Step 1. Let scope be the running execution context's
                //         LexicalEnvironment.
                // Step 2. Let funcEnv be NewDeclarativeEnvironment(scope).
                // Step 3. Let envRec be funcEnv's EnvironmentRecord.
                let mut data = LexicalScopeData::new_named_lambda(enclosing);

                // Step 4. Let name be StringValue of BindingIdentifier .
                // Step 5. Perform envRec.CreateImmutableBinding(name, false).
                let is_closed_over = self.base.name_tracker.is_closed_over_def(name);
                data.base
                    .bindings
                    .push(BindingName::new(*name, is_closed_over));

                ScopeData::Lexical(data)
            }
            None => ScopeData::Alias(enclosing),
        }
    }
}

/// The value of [[ThisMode]] internal slot of function object.
/// https://tc39.es/ecma262/#sec-ecmascript-function-objects
///
/// Defines how this references are interpreted within the formal parameters
/// and code body of the function.
#[derive(Debug, Clone, PartialEq)]
enum ThisMode {
    /// `this` refers to the `this` value of a lexically enclosing function.
    Lexical,

    /// `this` value is used exactly as provided by an invocation of the
    /// function.
    #[allow(dead_code)]
    Strict,

    /// `this` value of `undefined` is interpreted as a reference to the global
    /// object.
    Global,
}

/// The result of converting the builder of function parameters and body
/// into scope data.
struct FunctionScopeDataSet {
    /// ScopeData::Function.
    function: ScopeData,

    /// Either ScopeData::Var or ScopeData::Alias.
    extra_body_var: ScopeData,

    /// Either ScopeData::Lexical or ScopeData::Alias.
    lexical: ScopeData,
}

/// See FunctionParametersScopeBuilder.state.
#[derive(Debug)]
enum FunctionParametersState {
    /// Entered FormalParameters.
    Init,

    /// Entered Parameter.
    /// At this point, this parameter can be either non-destructuring or
    /// destructuring.
    /// If BindingIdentifier is found in this state, this parameter is
    /// non-destructuring.
    Parameter,

    /// Entered BindingPattern inside Parameter.
    /// This parameter is destructuring.
    DestructuringParameter,

    /// Entered rest parameter.
    /// At this point, the rest parameter can be either non-destructuring or
    /// destructuring.
    /// If BindingIdentifier is found in this state, the rest parameter is
    /// non-destructuring.
    RestParameter,

    /// Entered BindingPattern inside rest parameter.
    /// The rest parameter is destructuring.
    DestructuringRestParameter,
}

/// Function parameters in FormalParameters, and variables used in
/// FormalParameters
#[derive(Debug)]
struct FunctionParametersScopeBuilder {
    base: BaseScopeBuilder,

    /// State of the analysis.
    /// This is used to determine what kind of binding the parameter is.
    state: FunctionParametersState,

    /// List of positional parameter or None if destructuring.
    /// This includes rest parameter.
    positional_parameter_names: Vec<Option<SourceAtomSetIndex>>,

    /// List of non-positional parameters (destructuring parameters).
    non_positional_parameter_names: Vec<SourceAtomSetIndex>,

    /// FunctionDeclarationInstantiation ( func, argumentsList )
    /// https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
    ///
    /// Step 16. If func.[[ThisMode]] is lexical, then
    this_mode: ThisMode,

    /// Step 3. Let strict be func.[[Strict]].
    strict: bool,

    /// Step 5. Let parameterNames be the BoundNames of formals.
    ///
    /// NOTE: This is used only for checking duplication.
    ///       The actual list of parameters is stored in
    ///       positional_parameter_names and non_positional_parameter_names.
    parameter_names: HashSet<SourceAtomSetIndex>,

    /// Step 17. Else if "arguments" is an element of parameterNames, then
    parameter_has_arguments: bool,

    /// Step 6. If parameterNames has any duplicate entries, let hasDuplicates
    ///         be true. Otherwise, let hasDuplicates be false.
    has_duplicates: bool,

    /// Step 7. Let simpleParameterList be IsSimpleParameterList of formals.
    simple_parameter_list: bool,

    /// Step 8. Let hasParameterExpressions be ContainsExpression of formals.
    has_parameter_expressions: bool,

    scope_index: ScopeIndex,

    /// Index of the script in the list of `functions` in the
    /// `FunctionScriptStencilBuilder`.
    script_index: ScriptStencilIndex,

    has_direct_eval: bool,

    is_arrow: bool,
}

impl FunctionParametersScopeBuilder {
    fn new(scope_index: ScopeIndex, is_arrow: bool, script_index: ScriptStencilIndex) -> Self {
        let mut base = BaseScopeBuilder::new();

        if !is_arrow {
            // Arrow function closes over this/arguments from enclosing
            // function.
            base.name_tracker
                .note_def(CommonSourceAtomSetIndices::this());
            base.name_tracker
                .note_def(CommonSourceAtomSetIndices::arguments());
        }

        Self {
            base,

            state: FunctionParametersState::Init,

            positional_parameter_names: Vec::new(),
            non_positional_parameter_names: Vec::new(),

            // FIXME: Receive correct value.
            this_mode: ThisMode::Global,

            // FIMXE: Receive the enclosing strictness,
            //        and update on directive in body.
            strict: false,

            parameter_names: HashSet::new(),
            parameter_has_arguments: false,
            has_duplicates: false,
            simple_parameter_list: true,
            has_parameter_expressions: false,
            scope_index,
            script_index,
            has_direct_eval: false,
            is_arrow,
        }
    }

    fn before_parameter(&mut self) {
        match self.state {
            FunctionParametersState::Init => {
                self.state = FunctionParametersState::Parameter;
            }
            FunctionParametersState::Parameter => {
                self.state = FunctionParametersState::Parameter;
            }
            FunctionParametersState::DestructuringParameter => {
                self.state = FunctionParametersState::Parameter;
            }
            FunctionParametersState::RestParameter
            | FunctionParametersState::DestructuringRestParameter => panic!("Invalid transition"),
        }
    }

    fn before_binding_pattern(&mut self) {
        // Static Semantics: IsSimpleParameterList
        // https://tc39.es/ecma262/#sec-destructuring-binding-patterns-static-semantics-issimpleparameterlist
        //
        // BindingElement : BindingPattern
        //
        //   1. Return false.
        //
        // BindingElement : BindingPattern Initializer
        //
        //   1. Return false.
        self.simple_parameter_list = false;

        match self.state {
            FunctionParametersState::Parameter => {
                self.positional_parameter_names.push(None);
                self.state = FunctionParametersState::DestructuringParameter;
            }
            FunctionParametersState::DestructuringParameter => {}
            FunctionParametersState::RestParameter => {
                self.positional_parameter_names.push(None);
                self.state = FunctionParametersState::DestructuringRestParameter;
            }
            FunctionParametersState::DestructuringRestParameter => {}
            FunctionParametersState::Init => panic!("Invalid transition"),
        }
    }

    fn before_rest_parameter(&mut self) {
        // Static Semantics: IsSimpleParameterList
        // https://tc39.es/ecma262/#sec-function-definitions-static-semantics-issimpleparameterlist
        //
        // FormalParameters : FunctionRestParameter
        //
        //   1. Return false.
        //
        // FormalParameters : FormalParameterList , FunctionRestParameter
        //
        //   1. Return false.
        self.simple_parameter_list = false;

        match self.state {
            FunctionParametersState::Init
            | FunctionParametersState::Parameter
            | FunctionParametersState::DestructuringParameter => {
                self.state = FunctionParametersState::RestParameter;
            }
            FunctionParametersState::RestParameter
            | FunctionParametersState::DestructuringRestParameter => panic!("Invalid transition"),
        }
    }

    fn after_initializer(&mut self) {
        // Static Semantics: IsSimpleParameterList
        // https://tc39.es/ecma262/#sec-destructuring-binding-patterns-static-semantics-issimpleparameterlist
        //
        // BindingElement : BindingPattern Initializer
        //
        //   1. Return false.
        //
        // SingleNameBinding : BindingIdentifier Initializer
        //
        //   1. Return false.
        self.simple_parameter_list = false;

        // FunctionDeclarationInstantiation ( func, argumentsList )
        // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
        //
        // Step 8. Let hasParameterExpressions be ContainsExpression of formals.

        // Static Semantics: ContainsExpression
        // https://tc39.es/ecma262/#sec-destructuring-binding-patterns-static-semantics-containsexpression
        //
        // BindingElement : BindingPattern Initializer
        //
        //   1. Return true.
        //
        // SingleNameBinding : BindingIdentifier Initializer
        //
        //   1. Return true.
        self.has_parameter_expressions = true;
    }

    fn before_computed_property_name(&mut self) {
        // FunctionDeclarationInstantiation ( func, argumentsList )
        // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
        //
        // Step 8. Let hasParameterExpressions be ContainsExpression of formals.

        // Static Semantics: ContainsExpression
        // https://tc39.es/ecma262/#sec-destructuring-binding-patterns-static-semantics-containsexpression
        //
        // BindingProperty : PropertyName : BindingElement
        //
        //   1. Let has be IsComputedPropertyKey of PropertyName .
        //   2. If has is true, return true.
        //   3. Return ContainsExpression of BindingElement .
        self.has_parameter_expressions = true;
    }

    fn declare_param(&mut self, name: SourceAtomSetIndex) {
        // FunctionDeclarationInstantiation ( func, argumentsList )
        // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
        //
        // Step 5. Let parameterNames be the BoundNames of formals.
        match self.state {
            FunctionParametersState::Init => panic!("Invalid state"),
            FunctionParametersState::Parameter => {
                self.positional_parameter_names.push(Some(name.clone()));
            }
            FunctionParametersState::DestructuringParameter => {
                self.non_positional_parameter_names.push(name.clone());
            }
            FunctionParametersState::RestParameter => {
                self.positional_parameter_names.push(Some(name.clone()));
            }
            FunctionParametersState::DestructuringRestParameter => {
                self.non_positional_parameter_names.push(name.clone());
            }
        }

        // Step 6. If parameterNames has any duplicate entries, let
        //         hasDuplicates be true. Otherwise, let hasDuplicates be
        //         false.
        if self.parameter_names.contains(&name) {
            self.has_duplicates = true;
        }
        self.parameter_names.insert(name.clone());

        // Step 17. Else if "arguments" is an element of parameterNames,
        //          then
        if name == CommonSourceAtomSetIndices::arguments() {
            self.parameter_has_arguments = true;
        }
    }

    fn perform_annex_b(
        &mut self,
        scopes: &mut ScopeDataList,
        function_declaration_properties: &mut FunctionDeclarationPropertyMap,
        possibly_annex_b_functions: &mut PossiblyAnnexBFunctionList,
        body_scope_builder: &mut FunctionBodyScopeBuilder,
    ) {
        // Annex B
        // Changes to FunctionDeclarationInstantiation
        // https://tc39.es/ecma262/#sec-web-compat-functiondeclarationinstantiation
        //
        // Step 1. If strict is false, then
        //
        // FIXME: Once directives are supported, reflect it here.
        let strict = false;
        if strict {
            return;
        }

        // Step 1.a. For each FunctionDeclaration f that is directly contained
        //           in the StatementList of a Block, CaseClause, or
        //           DefaultClause, do
        //
        // NOTE: `possibly_annex_b_functions` contains all of them.

        // Step 1.a.i. Let F be StringValue of the BindingIdentifier of f.
        // Step 1.a.ii. If replacing the FunctionDeclaration f with a
        //              VariableStatement that has F as a BindingIdentifier
        //              would not produce any Early Errors for func and F is
        //              not an element of parameterNames, then
        //
        // NOTE: Early Errors happen if any of top-level lexical has
        //       the same name.  Filter out those functions here.
        for n in &body_scope_builder.let_names {
            possibly_annex_b_functions.remove_if_exists(*n);
        }
        for n in &body_scope_builder.const_names {
            possibly_annex_b_functions.remove_if_exists(*n);
        }

        // Step 1.a.ii.1. NOTE: A var binding for F is only instantiated here
        //                if it is neither a VarDeclaredName, the name of a
        //                formal parameter, or another FunctionDeclaration.
        //
        // NOTE: The binding is merged into the list of other var names.

        // Step 1.a.ii.2. If initializedBindings does not contain F and F is
        //                not "arguments", then
        possibly_annex_b_functions.remove_if_exists(CommonSourceAtomSetIndices::arguments());

        // Step 1.a.ii.2.a. Perform ! varEnv.CreateMutableBinding(F, false).
        // Step 1.a.ii.2.b. Perform varEnv.InitializeBinding(F, undefined).
        // Step 1.a.ii.2.c. Append F to instantiatedVarNames.
        for n in possibly_annex_b_functions.names() {
            body_scope_builder.var_names.insert(*n);
        }

        // Step 1.a.ii.3. When the FunctionDeclaration f is evaluated, perform
        //                the following steps in place of the
        //                FunctionDeclaration Evaluation algorithm provided in
        //                https://tc39.es/ecma262/#sec-function-definitions-runtime-semantics-evaluation
        // Step 1.a.ii.3.a. Let fenv be the running execution context's
        //                  VariableEnvironment.
        // Step 1.a.ii.3.b. Let benv be the running execution context's
        //                  LexicalEnvironment.
        // Step 1.a.ii.3.c. Let fobj be ! benv.GetBindingValue(F, false).
        // Step 1.a.ii.3.d. Perform ! fenv.SetMutableBinding(F, fobj, false).
        // Step 1.a.ii.3.e. Return NormalCompletion(empty).
        possibly_annex_b_functions.mark_annex_b(scopes, function_declaration_properties);
    }

    fn into_scope_data_set(
        mut self,
        scopes: &mut ScopeDataList,
        function_declaration_properties: &mut FunctionDeclarationPropertyMap,
        possibly_annex_b_functions: &mut PossiblyAnnexBFunctionList,
        enclosing: ScopeIndex,
        mut body_scope_builder: FunctionBodyScopeBuilder,
    ) -> FunctionScopeDataSet {
        // FunctionDeclarationInstantiation ( func, argumentsList )
        // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
        //
        // Step 15. Let argumentsObjectNeeded be true.
        let mut arguments_object_needed = true;

        // Step 16. If func.[[ThisMode]] is lexical, then
        if self.this_mode == ThisMode::Lexical {
            // Step 16.a. NOTE: Arrow functions never have an arguments objects.
            // Step 16.b. Set argumentsObjectNeeded to false.
            arguments_object_needed = false;
        }
        // Step 17. Else if "arguments" is an element of parameterNames,
        //          then
        else if self.parameter_has_arguments {
            // Step 17.a. Set argumentsObjectNeeded to false.
            arguments_object_needed = false;
        }
        // Step 18. Else if hasParameterExpressions is false, then
        else if !self.parameter_has_arguments {
            // Step 18.a. If "arguments" is an element of functionNames or if
            //            "arguments" is
            //            an element of lexicalNames, then
            if body_scope_builder.function_or_lexical_has_arguments {
                // Step 18.a.i. Set argumentsObjectNeeded to false.
                arguments_object_needed = false;
            }
        }

        // NOTE: In SpiderMonkey, single environment can have multiple
        //       binding kind.
        //       It's not necessary to create yet another environment here.
        //
        // Step 19. If strict is true or if hasParameterExpressions is false,
        //          then
        if self.strict || !self.has_parameter_expressions {
            // Step 19.a. NOTE: Only a single lexical environment is needed for
            //            the parameters and top-level vars.
            // Step 19.b. Let env be the LexicalEnvironment of calleeContext.
            // Step 19.c. Let envRec be env's EnvironmentRecord.
        }
        // Step 20. Else,
        else {
            // Step 20.a. NOTE: A separate Environment Record is needed to
            //            ensure that bindings created by direct eval calls in
            //            the formal parameter list are outside the environment
            //            where parameters are declared.
            // Step 20.b. Let calleeEnv be the LexicalEnvironment of
            //            calleeContext.
            // Step 20.c. Let env be NewDeclarativeEnvironment(calleeEnv).
            // Step 20.d. Let envRec be env's EnvironmentRecord.
            // Step 20.e. Assert: The VariableEnvironment of calleeContext is
            //            calleeEnv.
            // Step 20.f. Set the LexicalEnvironment of calleeContext to env.
        }

        // Step 29. NOTE: Annex B adds additional steps at this point.
        //
        // NOTE: Reordered here to reflect the change to
        //       body_scope_builder.var_names.
        self.perform_annex_b(
            scopes,
            function_declaration_properties,
            possibly_annex_b_functions,
            &mut body_scope_builder,
        );

        let has_extra_body_var_scope = self.has_parameter_expressions;

        // NOTE: Names in `body_scope_builder.var_names` is skipped if
        //       parameter has the same name, or it's `arguments`,
        //       at step 27.c.i.
        //       The count here isn't the exact number of var bindings, but
        //       it's fine given FunctionScopeData::new doesn't require the
        //       exact number, but just maximum number.
        let function_max_var_names_count = if has_extra_body_var_scope {
            0
        } else {
            body_scope_builder.var_names.len()
        };

        let mut function_scope_data = FunctionScopeData::new(
            self.has_parameter_expressions,
            self.positional_parameter_names.len(),
            self.non_positional_parameter_names.len(),
            function_max_var_names_count,
            enclosing,
            self.script_index,
            self.is_arrow,
        );

        // FunctionDeclarationInstantiation ( func, argumentsList )
        // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
        //
        // Step 21. For each String paramName in parameterNames, do
        // Step 21.a. Let alreadyDeclared be envRec.HasBinding(paramName).
        // Step 21.b. NOTE: Early errors ensure that duplicate parameter names
        //            can only occur in non-strict functions that do not have
        //            parameter default values or rest parameters.
        // Step 21.c. If alreadyDeclared is false, then
        // Step 21.c.i. Perform ! envRec.CreateMutableBinding(paramName, false).
        // Step 21.c.ii. If hasDuplicates is true, then
        // Step 21.c.ii.1. Perform
        //                 ! envRec.InitializeBinding(paramName, undefined).
        //
        // NOTE: The existence of duplication isn't encoded in scope data.
        for maybe_name in &self.positional_parameter_names {
            match maybe_name {
                Some(n) => {
                    let is_closed_over = self.base.name_tracker.is_closed_over_def(n)
                        || (!has_extra_body_var_scope
                            && body_scope_builder.base.name_tracker.is_closed_over_def(n));
                    function_scope_data
                        .base
                        .bindings
                        .push(Some(BindingName::new(*n, is_closed_over)))
                }
                None => function_scope_data.base.bindings.push(None),
            }
        }
        for n in &self.non_positional_parameter_names {
            let is_closed_over = self.base.name_tracker.is_closed_over_def(n)
                || (!has_extra_body_var_scope
                    && body_scope_builder.base.name_tracker.is_closed_over_def(n));
            function_scope_data
                .base
                .bindings
                .push(Some(BindingName::new(*n, is_closed_over)))
        }

        // Step 22. If argumentsObjectNeeded is true, then
        // Steps 22.a-b. Create{Unm,M}appedArgumentsObject
        // (done in emitter)

        // Step 22.c. If strict is true, then
        // Step 22.c.i. Perform
        //              ! envRec.CreateImmutableBinding("arguments", false).
        // Step 22.d. Else,
        // Step 22.d.i. Perform
        //              ! envRec.CreateMutableBinding("arguments", false).
        // Step 22.e. Call envRec.InitializeBinding("arguments", ao).
        //
        // NOTE: In SpiderMonkey, whether immutable or not is not stored
        //       in scope data, but checked while parsing, including
        //       when parsing eval inside function.

        // Step 22.f. Let parameterBindings be a new List of parameterNames
        //            with "arguments" appended.
        //
        // NOTE: Done in each consumer of parameterNames.

        // Step 23. Else,
        // Step 23.a. Let parameterBindings be parameterNames.
        //
        // NOTE: Done in each consumer of parameterNames.

        // Steps 24-26. IteratorBindingInitialization
        // (done in emitter)

        // Step 27. If hasParameterExpressions is false, then
        let extra_body_var_scope_data = if !self.has_parameter_expressions {
            debug_assert!(!has_extra_body_var_scope);

            // Step 27.a. NOTE: Only a single lexical environment is needed for
            //            the parameters and top-level vars.

            // Step 27.b. Let instantiatedVarNames be a copy of the List
            //            parameterBindings.
            // (implicit)

            // Step 27.c. For each n in varNames, do
            for n in &body_scope_builder.var_names {
                // Step 27.c.i. If n is not an element of instantiatedVarNames,
                //              then
                // Step 27.c.i.1. Append n to instantiatedVarNames.
                //
                // NOTE: var_names is already unique.
                //       Check against parameters here.
                if self.parameter_names.contains(n)
                    || (arguments_object_needed && *n == CommonSourceAtomSetIndices::arguments())
                {
                    continue;
                }

                // Step 27.c.i.2. Perform
                //                ! envRec.CreateMutableBinding(n, false).
                let is_closed_over = body_scope_builder.base.name_tracker.is_closed_over_def(n);
                function_scope_data
                    .base
                    .bindings
                    .push(Some(BindingName::new(*n, is_closed_over)));

                // Step 27.c.i.3. Call envRec.InitializeBinding(n, undefined).
                // (done in runtime)
            }

            // Step 27.d. Let varEnv be env.
            // Step 27.e. Let varEnvRec be envRec.
            ScopeData::Alias(self.scope_index)
        }
        // Step 28. Else,
        else {
            debug_assert!(has_extra_body_var_scope);

            // In non-strict mode code, direct `eval` can extend function's
            // scope.
            let function_has_extensible_scope = !self.strict && self.has_direct_eval;

            // Step 28.a. NOTE: A separate Environment Record is needed to
            //            ensure that closures created by expressions in the
            //            formal parameter list do not have visibility of
            //            declarations in the function body.

            // Step 28.b. Let varEnv be NewDeclarativeEnvironment(env).
            // Step 28.c. Let varEnvRec be varEnv's EnvironmentRecord.
            // Step 28.d. Set the VariableEnvironment of calleeContext to
            //            varEnv.
            let mut data = VarScopeData::new(
                body_scope_builder.var_names.len(),
                function_has_extensible_scope,
                /* encloding= */ self.scope_index,
            );

            // Step 28.e. Let instantiatedVarNames be a new empty List.
            // NOTE: var_names is already unique. Nothing to check here.

            // Step 28.f. For each n in varNames, do
            for n in &body_scope_builder.var_names {
                // Step 28.f.i. If n is not an element of instantiatedVarNames, then
                // Step 28.f.i.1. Append n to instantiatedVarNames.
                // (implicit)

                // Step 28.f.i.2. Perform
                //                ! varEnvRec.CreateMutableBinding(n, false).
                let is_closed_over = body_scope_builder.base.name_tracker.is_closed_over_def(n);
                data.base
                    .bindings
                    .push(BindingName::new(*n, is_closed_over));

                // Step 28.f.i.3. If n is not an element of parameterBindings or if
                //                n is an element of functionNames, let
                //                initialValue be undefined.
                // Step 28.f.i.4. Else,
                // Step 28.f.i.4.a. Let initialValue be
                //                  ! envRec.GetBindingValue(n, false).
                // Step 28.f.i.5. Call varEnvRec.InitializeBinding(n, initialValue).
                // (done in emitter)

                // Step 28.f.i.6. NOTE: A var with the same name as a formal
                //                parameter initially has the same value as the
                //                corresponding initialized parameter.
            }

            ScopeData::Var(data)
        };

        // Step 30. If strict is false, then
        // Step 30.a. Let lexEnv be NewDeclarativeEnvironment(varEnv).
        // Step 30.b. NOTE: Non-strict functions use a separate lexical
        //            Environment Record for top-level lexical declarations so
        //            that a direct eval can determine whether any var scoped
        //            declarations introduced by the eval code conflict with
        //            pre-existing top-level lexically scoped declarations.
        //            This is not needed for strict functions because a strict
        //            direct eval always places all declarations into a new
        //            Environment Record.
        // Step 31. Else, let lexEnv be varEnv.
        // Step 32. Let lexEnvRec be lexEnv's EnvironmentRecord.
        //
        // NOTE: SpiderMonkey creates lexical env whenever lexical binding
        //       exists.

        let lexical_scope_data =
            if body_scope_builder.let_names.len() > 0 || body_scope_builder.const_names.len() > 0 {
                let mut data = LexicalScopeData::new_function_lexical(
                    body_scope_builder.let_names.len(),
                    body_scope_builder.const_names.len(),
                    /* encloding= */ body_scope_builder.var_scope_index,
                );

                // Step 33. Set the LexicalEnvironment of calleeContext to lexEnv.
                // Step 34. Let lexDeclarations be the LexicallyScopedDeclarations
                //          of code.
                // Step 35. For each element d in lexDeclarations, do
                // Step 35.a. NOTE: A lexically declared name cannot be the same as
                //            a function/generator declaration, formal parameter,
                //            or a var name. Lexically declared names are only
                //            instantiated here but not initialized.
                // Step 35.b. For each element dn of the BoundNames of d, do

                for n in &body_scope_builder.let_names {
                    // Step 35.b.ii. Else,
                    // Step 35.b.ii.1. Perform
                    //                 ! lexEnvRec.CreateMutableBinding(dn, false).
                    let is_closed_over = body_scope_builder.base.name_tracker.is_closed_over_def(n);
                    data.base
                        .bindings
                        .push(BindingName::new(*n, is_closed_over))
                }
                for n in &body_scope_builder.const_names {
                    // Step 35.b.i. If IsConstantDeclaration of d is true, then
                    // Step 35.b.i.1. Perform
                    //                ! lexEnvRec.CreateImmutableBinding(dn, true).
                    let is_closed_over = body_scope_builder.base.name_tracker.is_closed_over_def(n);
                    data.base
                        .bindings
                        .push(BindingName::new(*n, is_closed_over))
                }

                ScopeData::Lexical(data)
            } else {
                ScopeData::Alias(body_scope_builder.var_scope_index)
            };

        // Step 36. For each Parse Node f in functionsToInitialize, do
        // (done in emitter)

        FunctionScopeDataSet {
            function: ScopeData::Function(function_scope_data),
            extra_body_var: extra_body_var_scope_data,
            lexical: lexical_scope_data,
        }
    }
}

/// Variables declared/used in FunctionBody.
#[derive(Debug)]
struct FunctionBodyScopeBuilder {
    base: BaseScopeBuilder,

    /// FunctionDeclarationInstantiation ( func, argumentsList )
    /// https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
    ///
    /// Step 9. Let varNames be the VarDeclaredNames of code.
    var_names: IndexSet<SourceAtomSetIndex>,

    /// Step 11. Let lexicalNames be the LexicallyDeclaredNames of code.
    let_names: Vec<SourceAtomSetIndex>,
    const_names: Vec<SourceAtomSetIndex>,

    /// Step 13. Let functionsToInitialize be a new empty List.
    functions_to_initialize: Vec<ScriptStencilIndex>,

    /// Step 18. Else if hasParameterExpressions is false, then
    /// Step 18.a. If "arguments" is an element of functionNames or
    ///            if "arguments" is an element of lexicalNames, then
    function_or_lexical_has_arguments: bool,

    var_scope_index: ScopeIndex,
    lexical_scope_index: ScopeIndex,
}

impl FunctionBodyScopeBuilder {
    fn new(var_scope_index: ScopeIndex, lexical_scope_index: ScopeIndex) -> Self {
        Self {
            base: BaseScopeBuilder::new(),
            var_names: IndexSet::new(),
            let_names: Vec::new(),
            const_names: Vec::new(),
            functions_to_initialize: Vec::new(),
            function_or_lexical_has_arguments: false,
            var_scope_index,
            lexical_scope_index,
        }
    }

    fn declare_var(&mut self, name: SourceAtomSetIndex) {
        // FunctionDeclarationInstantiation ( func, argumentsList )
        // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
        //
        // Step 9. Let varNames be the VarDeclaredNames of code.
        self.var_names.insert(name);
    }

    fn check_lexical_or_function_name(&mut self, name: SourceAtomSetIndex) {
        // FunctionDeclarationInstantiation ( func, argumentsList )
        // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
        //
        // Step 18.a. If "arguments" is an element of functionNames or if
        //            "arguments" is an element of lexicalNames, then
        if name == CommonSourceAtomSetIndices::arguments() {
            self.function_or_lexical_has_arguments = true;
        }
    }

    fn declare_let(&mut self, name: SourceAtomSetIndex) {
        // FunctionDeclarationInstantiation ( func, argumentsList )
        // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
        //
        // Step 11. Let lexicalNames be the LexicallyDeclaredNames of code.
        self.let_names.push(name.clone());

        self.check_lexical_or_function_name(name);
    }

    fn declare_const(&mut self, name: SourceAtomSetIndex) {
        // FunctionDeclarationInstantiation ( func, argumentsList )
        // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
        //
        // Step 11. Let lexicalNames be the LexicallyDeclaredNames of code.
        self.let_names.push(name.clone());

        self.check_lexical_or_function_name(name);
    }

    fn declare_function(&mut self, name: SourceAtomSetIndex, fun_index: ScriptStencilIndex) {
        // FunctionDeclarationInstantiation ( func, argumentsList )
        // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
        //
        // Step 14. For each d in varDeclarations, in reverse list order, do
        // Step 14.a. If d is neither a VariableDeclaration nor a ForBinding
        //            nor a BindingIdentifier , then
        // (implicit)

        // Step 14.a.i. Assert: d is either a FunctionDeclaration, a
        //              GeneratorDeclaration, an AsyncFunctionDeclaration,
        //              or an AsyncGeneratorDeclaration.

        // Step 14.a.ii. Let fn be the sole element of the BoundNames of d.

        // Step 14.a.iii. If fn is not an element of functionNames, then
        //
        // NOTE: Instead of iterating in reverse list oder, we iterate in
        // normal order and overwrite existing item.

        // Step 14.a.iii.1. Insert fn as the first element of functionNames.
        // Step 14.a.iii.2. NOTE: If there are multiple function declarations
        //                  for the same name, the last declaration is used.
        self.check_lexical_or_function_name(name);

        // Step 14.a.iii.3. Insert d as the first element of
        //                  functionsToInitialize.
        self.functions_to_initialize.push(fun_index);
    }
}

#[derive(Debug)]
enum ScopeBuilder {
    Global(GlobalScopeBuilder),
    Block(BlockScopeBuilder),
    FunctionExpression(FunctionExpressionScopeBuilder),
    FunctionParameters(FunctionParametersScopeBuilder),
    FunctionBody(FunctionBodyScopeBuilder),
}

impl ScopeBuilder {
    fn get_scope_index(&self) -> ScopeIndex {
        match self {
            ScopeBuilder::Global(builder) => builder.scope_index,
            ScopeBuilder::Block(builder) => builder.scope_index,
            ScopeBuilder::FunctionExpression(builder) => builder.scope_index,
            ScopeBuilder::FunctionParameters(builder) => builder.scope_index,
            ScopeBuilder::FunctionBody(builder) => builder.lexical_scope_index,
        }
    }

    fn declare_var(&mut self, name: SourceAtomSetIndex) {
        self.base_mut().name_tracker.note_def(name);

        match self {
            ScopeBuilder::Global(ref mut builder) => builder.declare_var(name),
            ScopeBuilder::FunctionBody(ref mut builder) => builder.declare_var(name),
            _ => panic!("unexpected var scope builder"),
        }
    }

    fn declare_let(&mut self, name: SourceAtomSetIndex) {
        self.base_mut().name_tracker.note_def(name);

        match self {
            ScopeBuilder::Global(ref mut builder) => builder.declare_let(name),
            ScopeBuilder::Block(ref mut builder) => builder.declare_let(name),
            ScopeBuilder::FunctionBody(ref mut builder) => builder.declare_let(name),
            _ => panic!("unexpected lexical scope builder"),
        }
    }

    fn declare_const(&mut self, name: SourceAtomSetIndex) {
        self.base_mut().name_tracker.note_def(name);

        match self {
            ScopeBuilder::Global(ref mut builder) => builder.declare_const(name),
            ScopeBuilder::Block(ref mut builder) => builder.declare_const(name),
            ScopeBuilder::FunctionBody(ref mut builder) => builder.declare_const(name),
            _ => panic!("unexpected lexical scope builder"),
        }
    }

    fn set_function_name(&mut self, name: SourceAtomSetIndex) {
        self.base_mut().name_tracker.note_def(name);

        match self {
            ScopeBuilder::FunctionExpression(ref mut builder) => builder.set_function_name(name),
            // FunctionDeclaration etc doesn't push any scope builder.
            // Just ignore.
            _ => {}
        }
    }

    fn declare_param(&mut self, name: SourceAtomSetIndex) {
        self.base_mut().name_tracker.note_def(name);

        match self {
            ScopeBuilder::FunctionParameters(ref mut builder) => builder.declare_param(name),
            _ => panic!("unexpected function scope builder"),
        }
    }

    fn base(&self) -> &BaseScopeBuilder {
        match self {
            ScopeBuilder::Global(builder) => &builder.base,
            ScopeBuilder::Block(builder) => &builder.base,
            ScopeBuilder::FunctionExpression(builder) => &builder.base,
            ScopeBuilder::FunctionParameters(builder) => &builder.base,
            ScopeBuilder::FunctionBody(builder) => &builder.base,
        }
    }

    fn base_mut(&mut self) -> &mut BaseScopeBuilder {
        match self {
            ScopeBuilder::Global(builder) => &mut builder.base,
            ScopeBuilder::Block(builder) => &mut builder.base,
            ScopeBuilder::FunctionExpression(builder) => &mut builder.base,
            ScopeBuilder::FunctionParameters(builder) => &mut builder.base,
            ScopeBuilder::FunctionBody(builder) => &mut builder.base,
        }
    }
}

/// Tracks what kind of binding the BindingIdentifier node corresponds to.
#[derive(Debug)]
struct ScopeKindStack {
    stack: Vec<ScopeKind>,
}

impl ScopeKindStack {
    fn new() -> Self {
        Self { stack: Vec::new() }
    }

    fn innermost<'a>(&'a self) -> &'a ScopeKind {
        self.stack
            .last()
            .expect("There should be at least one scope on the stack")
    }

    fn push(&mut self, kind: ScopeKind) {
        self.stack.push(kind)
    }

    fn pop(&mut self, kind: ScopeKind) {
        match self.stack.pop() {
            Some(k) if k == kind => {}
            _ => panic!("unmatching scope kind"),
        }
    }

    fn is_empty(&self) -> bool {
        self.stack.len() == 0
    }
}

/// The stack of scope builder for creating binding into.
#[derive(Debug)]
struct ScopeBuilderStack {
    stack: Vec<ScopeBuilder>,

    /// Stack of lists of names that is
    ///   1. defined in the scope
    ///   2. closed over by inner script
    ///
    /// Each list is delimited by `None`, for each scope.
    ///
    /// The order of scopes is depth-first post-order, and the order of names
    /// inside each scope is in not defined.
    ///
    /// When entering a function, empty list is pushed to this stack, and
    /// when leaving each function, top-most list is popped, and
    /// added to gcthings of the function, and this list is reset to empty.
    closed_over_bindings_for_lazy: Vec<Vec<Option<SourceAtomSetIndex>>>,
}

impl ScopeBuilderStack {
    fn new() -> Self {
        Self {
            stack: Vec::new(),
            closed_over_bindings_for_lazy: Vec::new(),
        }
    }

    fn innermost_var<'a>(&'a mut self) -> &'a mut ScopeBuilder {
        for builder in self.stack.iter_mut().rev() {
            match builder {
                ScopeBuilder::Global(_) => return builder,
                // NOTE: Function's body-level variable goes to
                // `FunctionBodyScopeBuilder`, regardless of the existence of
                // extra body var scope.
                // See `FunctionParametersScopeBuilder::into_scope_data_set`
                // for how those vars are stored into either function scope or
                // extra body var scope.
                ScopeBuilder::FunctionBody(_) => return builder,
                _ => {}
            }
        }

        panic!("There should be at least one scope on the stack");
    }

    fn maybe_innermost_function_parameters<'a>(
        &'a mut self,
    ) -> Option<&'a mut FunctionParametersScopeBuilder> {
        for builder in self.stack.iter_mut().rev() {
            match builder {
                ScopeBuilder::FunctionParameters(builder) => return Some(builder),
                _ => {}
            }
        }

        None
    }

    fn innermost_lexical<'a>(&'a mut self) -> &'a mut ScopeBuilder {
        // FIXME: If there's no other case, merge with innermost.
        self.innermost()
    }

    fn innermost<'a>(&'a mut self) -> &'a mut ScopeBuilder {
        self.stack
            .last_mut()
            .expect("There should be at least one scope on the stack")
    }

    fn current_scope_index(&self) -> ScopeIndex {
        self.stack
            .last()
            .expect("There should be at least one scope on the stack")
            .get_scope_index()
    }

    fn current_scope_index_or_empty_global(&self) -> ScopeIndex {
        self.current_scope_index()
    }

    fn push_global(&mut self, builder: GlobalScopeBuilder) {
        self.stack.push(ScopeBuilder::Global(builder))
    }

    fn pop_global(&mut self) -> GlobalScopeBuilder {
        match self.pop() {
            ScopeBuilder::Global(builder) => builder,
            _ => panic!("unmatching scope builder"),
        }
    }

    fn push_block(&mut self, builder: BlockScopeBuilder) {
        self.stack.push(ScopeBuilder::Block(builder))
    }

    fn pop_block(&mut self) -> BlockScopeBuilder {
        match self.pop() {
            ScopeBuilder::Block(builder) => builder,
            _ => panic!("unmatching scope builder"),
        }
    }

    fn push_function_expression(&mut self, builder: FunctionExpressionScopeBuilder) {
        self.stack.push(ScopeBuilder::FunctionExpression(builder))
    }

    fn pop_function_expression(&mut self) -> FunctionExpressionScopeBuilder {
        match self.pop() {
            ScopeBuilder::FunctionExpression(builder) => builder,
            _ => panic!("unmatching scope builder"),
        }
    }

    fn push_function_parameters(&mut self, builder: FunctionParametersScopeBuilder) {
        self.stack.push(ScopeBuilder::FunctionParameters(builder))
    }

    fn pop_function_parameters(&mut self) -> FunctionParametersScopeBuilder {
        match self.pop() {
            ScopeBuilder::FunctionParameters(builder) => builder,
            _ => panic!("unmatching scope builder"),
        }
    }

    fn get_function_parameters<'a>(&'a mut self) -> &'a mut FunctionParametersScopeBuilder {
        match self.innermost() {
            ScopeBuilder::FunctionParameters(builder) => builder,
            _ => panic!("unmatching scope builder"),
        }
    }

    fn push_function_body(&mut self, builder: FunctionBodyScopeBuilder) {
        self.stack.push(ScopeBuilder::FunctionBody(builder))
    }

    fn pop_function_body(&mut self) -> FunctionBodyScopeBuilder {
        match self.pop() {
            ScopeBuilder::FunctionBody(builder) => builder,
            _ => panic!("unmatching scope builder"),
        }
    }

    /// Pop the current scope, propagating names to outer scope.
    fn pop(&mut self) -> ScopeBuilder {
        let inner = self.stack.pop().expect("unmatching scope builder");
        match self.stack.last_mut() {
            Some(outer) => {
                let inner_base = inner.base();
                let outer_base = outer.base_mut();

                match self.closed_over_bindings_for_lazy.last_mut() {
                    Some(bindings) => {
                        for name in inner_base.name_tracker.defined_and_closed_over_vars() {
                            bindings.push(Some(*name));
                        }
                        bindings.push(None);
                    }
                    None => {
                        // We're leaving lexical scope in top-level script.
                    }
                }

                // When construct such as `eval`, `with` and `delete` access
                // name dynamically in inner scopes, we have to propagate this
                // flag to the outer scope such that we prevent optimizations.
                outer_base.bindings_accessed_dynamically |=
                    inner_base.bindings_accessed_dynamically;

                match inner {
                    ScopeBuilder::Global(_) => {
                        panic!("Global shouldn't be enclosed by other scope");
                    }
                    ScopeBuilder::Block(_) => {
                        outer_base
                            .name_tracker
                            .propagate_from_inner_non_script(&inner_base.name_tracker);
                    }
                    ScopeBuilder::FunctionExpression(_) => {
                        // NOTE: Function expression's name cannot have any
                        //       used free variables.
                        //       We can treat it as non-script here, so that
                        //       any closed-over free variables inside this
                        //       function is propagated from FunctionParameters
                        //       to enclosing scope builder.
                        outer_base
                            .name_tracker
                            .propagate_from_inner_non_script(&inner_base.name_tracker);
                    }
                    ScopeBuilder::FunctionParameters(_) => {
                        outer_base
                            .name_tracker
                            .propagate_from_inner_script(&inner_base.name_tracker);
                    }
                    ScopeBuilder::FunctionBody(_) => {
                        outer_base
                            .name_tracker
                            .propagate_from_inner_non_script(&inner_base.name_tracker);
                    }
                }
            }
            None => {}
        }
        inner
    }
}

/// Builds `ScriptStencil` for all functions (both non-lazy and lazy).
/// The script is set to lazy function, with inner functions and
/// closed over bindings populated in gcthings list.
///
/// TODO: For non-lazy function, gcthings list should be populated in the
///       emitter pass, not here.
#[derive(Debug)]
pub struct FunctionScriptStencilBuilder {
    /// The map from function node to ScriptStencil.
    ///
    /// The map is separated into `function_stencil_indicies` and `functions`,
    /// because it can be referred to in different ways from multiple places:
    ///   * map from Function AST node (`function_stencil_indices`)
    ///   * enclosing script/function, to list inner functions
    function_stencil_indices: AssociatedData<ScriptStencilIndex>,
    functions: ScriptStencilList,

    /// The stack of functions that the current context is in.
    ///
    /// The last element in this stack represents the current function, where
    /// the inner function will be stored
    function_stack: Vec<ScriptStencilIndex>,
}

impl FunctionScriptStencilBuilder {
    fn new() -> Self {
        Self {
            function_stencil_indices: AssociatedData::new(),
            functions: ScriptStencilList::new(),
            function_stack: Vec::new(),
        }
    }

    /// Enter a function.
    ///
    /// This creates `ScriptStencil` for the function, and adds it to
    /// enclosing function if exists.
    fn enter<T>(
        &mut self,
        fun: &T,
        syntax_kind: FunctionSyntaxKind,
        enclosing_scope_index: ScopeIndex,
    ) -> ScriptStencilIndex
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        let loc = fun.get_loc();
        let source_start = loc.start as u32;

        // FIXME: Map from offset to line/column.
        let lineno = 1;
        let column = 0;

        let function_stencil = ScriptStencil::lazy_function(
            SourceExtent {
                source_start,
                source_end: 0,
                to_string_start: source_start,
                to_string_end: 0,
                lineno,
                column,
            },
            None,
            syntax_kind.is_generator(),
            syntax_kind.is_async(),
            FunctionFlags::interpreted(syntax_kind),
            enclosing_scope_index,
        );
        let index = self.functions.push(function_stencil);
        self.function_stencil_indices.insert(fun, index);

        match self.maybe_current_mut() {
            Some(enclosing) => {
                enclosing.push_inner_function(index);
            }
            None => {}
        }

        self.function_stack.push(index);

        index
    }

    /// Leave a function, setting its source location.
    fn leave<T>(&mut self, fun: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        let loc = fun.get_loc();
        let source_end = loc.end;

        self.current_mut().set_source_end(source_end);
        self.current_mut().set_to_string_end(source_end);

        self.function_stack.pop();
    }

    /// Returns the current function's index.
    /// Panics if no current function is found.
    fn current_index(&self) -> ScriptStencilIndex {
        *self
            .function_stack
            .last()
            .expect("should be inside function")
    }

    /// Returns a immutable reference to the innermost function. None otherwise.
    fn maybe_current<'a>(&'a self) -> Option<&'a ScriptStencil> {
        let maybe_index = self.function_stack.last();
        maybe_index.map(move |index| self.functions.get(*index))
    }

    /// Returns a immutable reference to the current function.
    /// Panics if no current function is found.
    fn current<'a>(&'a self) -> &'a ScriptStencil {
        self.maybe_current().expect("should be inside function")
    }

    /// Returns a mutable reference to the innermost function. None otherwise.
    fn maybe_current_mut<'a>(&'a mut self) -> Option<&'a mut ScriptStencil> {
        let maybe_index = self.function_stack.last().cloned();
        maybe_index.map(move |index| self.functions.get_mut(index))
    }

    /// Returns a mutable reference to the current function.
    /// Panics if no current function is found.
    fn current_mut<'a>(&'a mut self) -> &'a mut ScriptStencil {
        self.maybe_current_mut().expect("should be inside function")
    }

    /// Sets the name of the current function.
    fn set_function_name(&mut self, name: SourceAtomSetIndex) {
        self.current_mut().set_fun_name(name);
    }

    /// Sets the position of the function parameters.
    /// `params` should point to the `(` of the function parameters.
    /// If it's an arrow function without parenthesis, `params` should point
    /// the parameter binding.
    fn on_function_parameters<T>(&mut self, params: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        let loc = params.get_loc();
        let params_start = loc.start;
        self.current_mut().set_source_starts(params_start);
    }

    fn on_non_rest_parameter(&mut self) {
        let fun = self.current_mut();
        fun.add_fun_nargs();
    }

    /// Flags that the current function has rest parameter.
    fn on_rest_parameter(&mut self) {
        let fun = self.current_mut();
        fun.add_fun_nargs();
        fun.set_has_rest();
    }

    fn add_closed_over_bindings(
        &mut self,
        mut closed_over_bindings_for_lazy: Vec<Option<SourceAtomSetIndex>>,
    ) {
        // Remove trailing `None`s.
        loop {
            match closed_over_bindings_for_lazy.last() {
                Some(Some(_)) => {
                    // The last item isn't None.
                    break;
                }

                Some(None) => {
                    // The last item is None, remove it
                    closed_over_bindings_for_lazy.pop();
                }

                None => {
                    // List is empty.
                    break;
                }
            }
        }

        let current = self.current_mut();
        for name in closed_over_bindings_for_lazy {
            match name {
                Some(name) => current.push_closed_over_bindings(name),
                None => current.push_closed_over_bindings_delimiter(),
            }
        }
    }
}

/// Receives method calls telling about a JS script and builds a
/// `ScopeDataMap`.
///
/// Usage: This struct's public methods must be called for each scope,
/// declaration, and identifier in a JS script, in source order. Then use
/// `ScopeDataMap::from()` to extract the results. Currently this object is
/// driven by method calls from a `pass::ScopePass`.
#[derive(Debug)]
pub struct ScopeDataMapBuilder {
    scope_kind_stack: ScopeKindStack,
    builder_stack: ScopeBuilderStack,
    scopes: ScopeDataList,

    /// The global scope information.
    /// Using `Option` to make this field populated later.
    global: Option<ScopeIndex>,

    /// The map from non-global AST node to scope information.
    non_global: AssociatedData<ScopeIndex>,

    function_stencil_builder: FunctionScriptStencilBuilder,

    function_declaration_properties: FunctionDeclarationPropertyMap,

    possibly_annex_b_functions: PossiblyAnnexBFunctionList,
}

impl ScopeDataMapBuilder {
    pub fn new() -> Self {
        Self {
            scope_kind_stack: ScopeKindStack::new(),
            builder_stack: ScopeBuilderStack::new(),
            scopes: ScopeDataList::new(),
            global: None,
            non_global: AssociatedData::new(),
            function_stencil_builder: FunctionScriptStencilBuilder::new(),
            function_declaration_properties: FunctionDeclarationPropertyMap::new(),
            possibly_annex_b_functions: PossiblyAnnexBFunctionList::new(),
        }
    }

    pub fn before_script(&mut self) {
        // SetRealmGlobalObject ( realmRec, globalObj, thisValue )
        // https://tc39.es/ecma262/#sec-setrealmglobalobject
        //
        // Steps 1-4, 7.
        // (done in runtime)

        // Step 5. Let newGlobalEnv be
        //         NewGlobalEnvironment(globalObj, thisValue).
        let index = self.scopes.allocate();
        let builder = GlobalScopeBuilder::new(index);
        self.global = Some(index);

        // Step 6. Set realmRec.[[GlobalEnv]] to newGlobalEnv.
        // (implicit)

        // ScriptEvaluation ( scriptRecord )
        // https://tc39.es/ecma262/#sec-runtime-semantics-scriptevaluation
        //
        // Step 1. Let globalEnv be scriptRecord.[[Realm]].[[GlobalEnv]].
        // (implicit)

        // Step 2. Let scriptContext be a new ECMAScript code execution context.
        // (implicit)

        // Steps 3-5.
        // (done in runtime)

        // Step 6. Set the VariableEnvironment of scriptContext to globalEnv.
        // Step 7. Set the LexicalEnvironment of scriptContext to globalEnv.
        self.builder_stack.push_global(builder);

        // Steps 8-17.
        // (done in runtime)
    }

    pub fn after_script(&mut self) {
        let builder = self.builder_stack.pop_global();

        // Runtime Semantics: GlobalDeclarationInstantiation ( script, env )
        // https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
        //
        // Steps 3-6.
        // (done in runtime)

        // Steps 12-18.
        let scope_index = builder.scope_index;
        let scope = builder.into_scope_data(
            &mut self.scopes,
            &mut self.function_declaration_properties,
            &mut self.possibly_annex_b_functions,
        );
        self.scopes.populate(scope_index, scope);
    }

    pub fn before_block_statement<T>(&mut self, block: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        // Runtime Semantics: Evaluation
        // https://tc39.es/ecma262/#sec-block-runtime-semantics-evaluation
        //
        // Block : { StatementList }
        //
        // Step 1. Let oldEnv be the running execution context's
        //         LexicalEnvironment.
        // (implicit)

        // Step 2. Let blockEnv be NewDeclarativeEnvironment(oldEnv).
        let index = self.scopes.allocate();
        let builder = BlockScopeBuilder::new(index);
        self.non_global.insert(block, index);

        // Step 3. Perform
        //         BlockDeclarationInstantiation(StatementList, blockEnv).
        // (done in leave_enum_statement_variant_block_statement)

        // Step 4. Set the running execution context's LexicalEnvironment to
        //         blockEnv.
        self.builder_stack.push_block(builder);

        // Step 5. Let blockValue be the result of evaluating StatementList.
        // (done in runtime)
    }

    pub fn after_block_statement(&mut self) {
        let builder = self.builder_stack.pop_block();
        let enclosing = self.builder_stack.current_scope_index();

        // Runtime Semantics: Evaluation
        // https://tc39.es/ecma262/#sec-block-runtime-semantics-evaluation
        //
        // Block : { StatementList }
        //
        // Step 3. Perform
        //         BlockDeclarationInstantiation(StatementList, blockEnv).
        self.scopes.populate(
            builder.scope_index,
            builder.into_scope_data(enclosing, &mut self.possibly_annex_b_functions),
        );

        // Step 6. Set the running execution context's LexicalEnvironment to
        //         oldEnv.

        // Step 7. Return blockValue.
        // (implicit)
    }

    pub fn before_var_declaration(&mut self) {
        self.scope_kind_stack.push(ScopeKind::Var);
    }

    pub fn after_var_declaration(&mut self) {
        self.scope_kind_stack.pop(ScopeKind::Var);
    }

    pub fn before_let_declaration(&mut self) {
        self.scope_kind_stack.push(ScopeKind::Let);
    }

    pub fn after_let_declaration(&mut self) {
        self.scope_kind_stack.pop(ScopeKind::Let);
    }

    pub fn before_const_declaration(&mut self) {
        self.scope_kind_stack.push(ScopeKind::Const);
    }

    pub fn after_const_declaration(&mut self) {
        self.scope_kind_stack.pop(ScopeKind::Const);
    }

    pub fn on_binding_identifier(&mut self, name: SourceAtomSetIndex) {
        if self.scope_kind_stack.is_empty() {
            // FIXME
            // Do nothing for unsupported case.
            // Emitter will return NotImplemented anyway.
            return;
        }

        match self.scope_kind_stack.innermost() {
            ScopeKind::Var => self.builder_stack.innermost_var().declare_var(name),
            ScopeKind::Let => self.builder_stack.innermost_lexical().declare_let(name),
            ScopeKind::Const => self.builder_stack.innermost_lexical().declare_const(name),
            ScopeKind::FunctionName => {
                self.builder_stack.innermost().set_function_name(name);
                self.function_stencil_builder.set_function_name(name);
            }
            ScopeKind::FormalParameter => self.builder_stack.innermost().declare_param(name),
            _ => panic!("Not implemeneted"),
        }
    }

    pub fn on_non_binding_identifier(&mut self, name: SourceAtomSetIndex) {
        self.builder_stack
            .innermost()
            .base_mut()
            .name_tracker
            .note_use(name);
    }

    pub fn before_function_declaration<T>(
        &mut self,
        name: SourceAtomSetIndex,
        fun: &T,
        is_generator: bool,
        is_async: bool,
    ) -> ScriptStencilIndex
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        let fun_index = self.function_stencil_builder.enter(
            fun,
            FunctionSyntaxKind::function_declaration(is_generator, is_async),
            self.builder_stack.current_scope_index_or_empty_global(),
        );

        match self.builder_stack.innermost_lexical() {
            ScopeBuilder::Global(ref mut builder) => builder.declare_function(name, fun_index),
            ScopeBuilder::Block(ref mut builder) => builder.declare_function(name, fun_index),
            ScopeBuilder::FunctionBody(ref mut builder) => {
                builder.declare_function(name, fun_index)
            }
            _ => panic!("unexpected lexical for FunctionDeclaration"),
        }

        self.scope_kind_stack.push(ScopeKind::FunctionName);

        fun_index
    }

    pub fn after_function_declaration<T>(&mut self, fun: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        self.function_stencil_builder.leave(fun);

        self.scope_kind_stack.pop(ScopeKind::FunctionName);
    }

    pub fn before_function_expression<T>(&mut self, fun: &T, is_generator: bool, is_async: bool)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        let index = self.scopes.allocate();
        let builder = FunctionExpressionScopeBuilder::new(index);
        self.non_global.insert(fun, index);

        self.builder_stack.push_function_expression(builder);

        self.scope_kind_stack.push(ScopeKind::FunctionName);

        self.function_stencil_builder.enter(
            fun,
            FunctionSyntaxKind::function_expression(is_generator, is_async),
            self.builder_stack.current_scope_index_or_empty_global(),
        );
    }

    pub fn after_function_expression<T>(&mut self, fun: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        self.function_stencil_builder.leave(fun);

        self.scope_kind_stack.pop(ScopeKind::FunctionName);

        let builder = self.builder_stack.pop_function_expression();
        let enclosing = self.builder_stack.current_scope_index();

        self.scopes
            .populate(builder.scope_index, builder.into_scope_data(enclosing));
    }

    pub fn before_method<T>(&mut self, fun: &T, is_generator: bool, is_async: bool)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        self.function_stencil_builder.enter(
            fun,
            FunctionSyntaxKind::method(is_generator, is_async),
            self.builder_stack.current_scope_index_or_empty_global(),
        );
    }

    pub fn after_method<T>(&mut self, fun: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        self.function_stencil_builder.leave(fun);
    }

    pub fn before_getter<T>(&mut self, fun: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        self.function_stencil_builder.enter(
            fun,
            FunctionSyntaxKind::getter(),
            self.builder_stack.current_scope_index_or_empty_global(),
        );
    }

    pub fn on_getter_parameter<T>(&mut self, param: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        self.before_function_parameters(param);
        self.after_function_parameters();
    }

    pub fn after_getter<T>(&mut self, fun: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        self.function_stencil_builder.leave(fun);
    }

    pub fn before_setter<T>(&mut self, fun: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        self.function_stencil_builder.enter(
            fun,
            FunctionSyntaxKind::setter(),
            self.builder_stack.current_scope_index_or_empty_global(),
        );
    }

    pub fn before_setter_parameter<T>(&mut self, param: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        self.before_function_parameters(param);
        self.before_parameter();
    }

    pub fn after_setter_parameter(&mut self) {
        self.after_function_parameters();
    }

    pub fn after_setter<T>(&mut self, fun: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        self.function_stencil_builder.leave(fun);
    }

    pub fn before_arrow_function<T>(&mut self, is_async: bool, params: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        self.function_stencil_builder.enter(
            params,
            FunctionSyntaxKind::arrow(is_async),
            self.builder_stack.current_scope_index_or_empty_global(),
        );
    }

    pub fn after_arrow_function<T>(&mut self, body: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        self.function_stencil_builder.leave(body);
    }

    pub fn before_function_parameters<T>(&mut self, params: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        self.builder_stack
            .closed_over_bindings_for_lazy
            .push(Vec::new());

        self.function_stencil_builder.on_function_parameters(params);

        let index = self.scopes.allocate();

        let is_arrow = self.function_stencil_builder.current().is_arrow_function();

        let builder = FunctionParametersScopeBuilder::new(
            index,
            is_arrow,
            self.function_stencil_builder.current_index(),
        );
        self.non_global.insert(params, index);

        self.builder_stack.push_function_parameters(builder);
        self.scope_kind_stack.push(ScopeKind::FormalParameter);
    }

    pub fn after_function_parameters(&mut self) {
        self.scope_kind_stack.pop(ScopeKind::FormalParameter);
    }

    pub fn before_parameter(&mut self) {
        let builder = self.builder_stack.get_function_parameters();
        builder.before_parameter();

        self.function_stencil_builder.on_non_rest_parameter();
    }

    pub fn before_binding_pattern(&mut self) {
        match self.builder_stack.innermost() {
            ScopeBuilder::FunctionParameters(builder) => {
                builder.before_binding_pattern();
            }
            _ => {}
        }
    }

    pub fn after_initializer(&mut self) {
        match self.builder_stack.innermost() {
            ScopeBuilder::FunctionParameters(builder) => {
                builder.after_initializer();
            }
            _ => {}
        }
    }

    pub fn before_computed_property_name(&mut self) {
        match self.builder_stack.innermost() {
            ScopeBuilder::FunctionParameters(builder) => {
                builder.before_computed_property_name();
            }
            _ => {}
        }
    }

    pub fn before_rest_parameter(&mut self) {
        self.function_stencil_builder.on_rest_parameter();

        let builder = self.builder_stack.get_function_parameters();
        builder.before_rest_parameter();
    }

    pub fn before_function_body<T>(&mut self, body: &T)
    where
        T: SourceLocationAccessor + NodeTypeIdAccessor,
    {
        let var_index = self.scopes.allocate();
        let lexical_index = self.scopes.allocate();
        debug_assert!(lexical_index == var_index.next());

        let builder = FunctionBodyScopeBuilder::new(var_index, lexical_index);
        self.non_global.insert(body, var_index);

        self.builder_stack.push_function_body(builder);
    }

    pub fn after_function_body(&mut self) {
        let body_scope_builder = self.builder_stack.pop_function_body();
        let parameter_scope_builder = self.builder_stack.pop_function_parameters();
        let enclosing = self.builder_stack.current_scope_index();

        self.function_stencil_builder.add_closed_over_bindings(
            self.builder_stack
                .closed_over_bindings_for_lazy
                .pop()
                .expect("Vector should be pushed by before_function_parameters"),
        );

        let function_scope_index = parameter_scope_builder.scope_index;
        let var_scope_index = body_scope_builder.var_scope_index;
        let lexical_scope_index = body_scope_builder.lexical_scope_index;

        // Save scope information used by FunctionScriptStencilBuilder into
        // local variables here before consuming ScopeBuilders.
        let bindings_accessed_dynamically =
            parameter_scope_builder.base.bindings_accessed_dynamically;

        let has_used_this = parameter_scope_builder
            .base
            .name_tracker
            .is_used_or_closed_over(CommonSourceAtomSetIndices::this())
            || bindings_accessed_dynamically;
        let has_used_arguments = parameter_scope_builder
            .base
            .name_tracker
            .is_used_or_closed_over(CommonSourceAtomSetIndices::arguments())
            || bindings_accessed_dynamically;

        let parameter_has_arguments = parameter_scope_builder.parameter_has_arguments;

        // NOTE: `var` here doesn't include Annex B functions.
        //       Also, `var_names_has_arguments` becomes true regardless of
        //       `arguments` in parameter.
        let var_names_has_arguments = body_scope_builder
            .var_names
            .contains(&CommonSourceAtomSetIndices::arguments());

        let body_has_defined_arguments =
            var_names_has_arguments || body_scope_builder.function_or_lexical_has_arguments;

        let strict = parameter_scope_builder.strict;
        let simple_parameter_list = parameter_scope_builder.simple_parameter_list;
        let has_mapped_arguments = !strict && simple_parameter_list;

        // Runtime Semantics: EvaluateBody
        // https://tc39.es/ecma262/#sec-function-definitions-runtime-semantics-evaluatebody
        //
        // With parameters functionObject and List argumentsList.
        //
        // FunctionBody : FunctionStatementList
        //
        // Step 1. Perform ? FunctionDeclarationInstantiation(functionObject,
        //         argumentsList).
        let scope_data_set = parameter_scope_builder.into_scope_data_set(
            &mut self.scopes,
            &mut self.function_declaration_properties,
            &mut self.possibly_annex_b_functions,
            enclosing,
            body_scope_builder,
        );
        self.possibly_annex_b_functions.clear();

        let has_extra_body_var = match &scope_data_set.extra_body_var {
            ScopeData::Var(_) => true,
            _ => false,
        };

        let fun_stencil = self.function_stencil_builder.current_mut();

        if let ScopeData::Function(fun) = &scope_data_set.function {
            if fun.base.needs_environment_object() {
                fun_stencil.set_needs_function_environment_objects();
            }
        } else {
            panic!("Unexpected scope data for function");
        }

        if has_extra_body_var {
            fun_stencil.set_function_has_extra_body_var_scope();
        }

        if has_mapped_arguments {
            fun_stencil.set_has_mapped_args_obj();
        }

        if !fun_stencil.is_arrow_function() {
            if has_used_this {
                fun_stencil.set_function_has_this_binding();
            }

            let mut uses_arguments = false;
            let mut try_declare_arguments = has_used_arguments;

            // FunctionDeclarationInstantiation ( func, argumentsList )
            // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
            //
            // Step 17 (Else if "arguments" is an element of parameterNames...)
            // and step 18 (Else if hasParameterExpressions is false...) say
            // formal parameters, lexical bindings, and body-level functions
            // named 'arguments' shadow the arguments object.
            //
            // So even if there wasn't a free use of 'arguments' but there is a
            // var binding of 'arguments', we still might need the arguments
            // object.
            //
            // If we have an extra var scope due to parameter expressions and
            // the body declared 'var arguments', we still need to declare
            // 'arguments' in the function scope.
            //
            // NOTE: This is implementation-specfic optimization, and has
            //       no corresponding steps in the spec.
            if var_names_has_arguments {
                if has_extra_body_var {
                    try_declare_arguments = true;
                } else if !parameter_has_arguments {
                    uses_arguments = true;
                }
            }

            if try_declare_arguments {
                // if extra body var scope exists, the existence of `arguments`
                // binding in function body doesn't affect.
                let declare_arguments =
                    !parameter_has_arguments && (has_extra_body_var || !body_has_defined_arguments);

                if declare_arguments {
                    fun_stencil.set_should_declare_arguments();
                    uses_arguments = true;
                }
            }

            if uses_arguments {
                // There is an 'arguments' binding. Is the arguments object
                // definitely needed?
                fun_stencil.set_arguments_has_var_binding();

                // Dynamic scope access destroys all hope of optimization.
                if bindings_accessed_dynamically {
                    fun_stencil.set_always_needs_args_obj();
                }
            }
        }

        self.scopes
            .populate(function_scope_index, scope_data_set.function);
        self.scopes
            .populate(var_scope_index, scope_data_set.extra_body_var);
        self.scopes
            .populate(lexical_scope_index, scope_data_set.lexical);
    }

    #[allow(dead_code)]
    pub fn on_direct_eval(&mut self) {
        if let Some(parameter_scope_builder) =
            self.builder_stack.maybe_innermost_function_parameters()
        {
            parameter_scope_builder.has_direct_eval = true;
        }

        self.builder_stack
            .innermost()
            .base_mut()
            .bindings_accessed_dynamically = true;
    }
}

pub struct ScopeDataMapAndScriptStencilList {
    pub scope_data_map: ScopeDataMap,
    pub function_stencil_indices: AssociatedData<ScriptStencilIndex>,
    pub function_declaration_properties: FunctionDeclarationPropertyMap,
    pub functions: ScriptStencilList,
}

impl From<ScopeDataMapBuilder> for ScopeDataMapAndScriptStencilList {
    fn from(builder: ScopeDataMapBuilder) -> ScopeDataMapAndScriptStencilList {
        ScopeDataMapAndScriptStencilList {
            scope_data_map: ScopeDataMap::new(
                builder.scopes,
                builder.global.expect("There should be global scope data"),
                builder.non_global,
            ),
            function_stencil_indices: builder.function_stencil_builder.function_stencil_indices,
            function_declaration_properties: builder.function_declaration_properties,
            functions: builder.function_stencil_builder.functions,
        }
    }
}
