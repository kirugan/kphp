#pragma once

#include "compiler/function-pass.h"
#include "compiler/pipes/function-and-cfg.h"

class CloneStrangeConstParams : public FunctionPassBase {
public:
  using ExecuteType = FunctionAndCFG;

  VertexPtr on_enter_vertex(VertexPtr root, LocalT *);

  bool need_recursion(VertexPtr v, LocalT *) {
    return v->type() != op_func_call;
  }
};