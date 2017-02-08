#ifndef TACO_AVAILABLE_EXPRS_H
#define TACO_AVAILABLE_EXPRS_H

#include <vector>

#include "expr.h"

namespace taco {
class Var;

namespace lower {

/// Retrieves available sub-expression, which are the maximal sub-expressions
/// whose operands are only indexed by the given index variables.
std::vector<taco::Expr>
getAvailableExpressions(const taco::Expr& expr,
                        const std::vector<taco::Var>& vars);

}}
#endif
