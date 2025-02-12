// |reftest| skip error:SyntaxError -- class-methods-private is not supported
// This file was procedurally generated from the following sources:
// - src/class-elements/err-delete-call-expression-private-method-async-gen.case
// - src/class-elements/delete-error/cls-decl-field-delete-covered.template
/*---
description: It's a SyntaxError if delete operator is applied to CallExpression.PrivateName async generator (in field, covered)
esid: sec-class-definitions-static-semantics-early-errors
features: [class-methods-private, async-iteration, class, class-fields-private, class-fields-public]
flags: [generated]
negative:
  phase: parse
  type: SyntaxError
info: |
    Static Semantics: Early Errors

      UnaryExpression : delete UnaryExpression

      It is a Syntax Error if the UnaryExpression is contained in strict mode
      code and the derived UnaryExpression is
      PrimaryExpression : IdentifierReference ,
      MemberExpression : MemberExpression.PrivateName , or
      CallExpression : CallExpression.PrivateName .

      It is a Syntax Error if the derived UnaryExpression is
      PrimaryExpression : CoverParenthesizedExpressionAndArrowParameterList and
      CoverParenthesizedExpressionAndArrowParameterList ultimately derives a
      phrase that, if used in place of UnaryExpression, would produce a
      Syntax Error according to these rules. This rule is recursively applied.

---*/


$DONOTEVALUATE();

class C {
  #x;
  g = this.f;
  x = delete (g().#m);
  f() {
  return this;
  }
}
