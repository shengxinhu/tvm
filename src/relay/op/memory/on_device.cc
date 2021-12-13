/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 *
 * \file src/relay/op/memory/on_device.cc
 * \brief Helpers for working with the "on_device" 'annotation' call.
 */

#include "./on_device.h"

#include <tvm/relay/attrs/annotation.h>
#include <tvm/relay/expr.h>
#include <tvm/relay/op.h>
#include <tvm/relay/op_attr_types.h>

#include "../../transforms/infer_layout_utils.h"
#include "../type_relations.h"

namespace tvm {
namespace relay {

TVM_REGISTER_NODE_TYPE(OnDeviceAttrs);

const Op& OnDeviceOp() {
  static const Op& op = Op::Get("on_device");
  return op;
}

Call OnDevice(Expr body, SEScope se_scope, bool constrain_result, bool constrain_body) {
  ICHECK((!constrain_result && !constrain_body) || !se_scope->IsFullyUnconstrained());
  auto attrs = make_object<OnDeviceAttrs>();
  attrs->se_scope =
      (constrain_result || constrain_body) ? std::move(se_scope) : SEScope::FullyUnconstrained();
  attrs->constrain_result = constrain_result;
  attrs->constrain_body = constrain_body;
  Span span = body->span;  // about to be moved
  return Call(OnDeviceOp(), {std::move(body)}, Attrs(std::move(attrs)), /*type_args=*/{},
              std::move(span));
}

TVM_REGISTER_GLOBAL("relay.op.annotation._make.OnDevice").set_body_typed(OnDevice);

Expr MaybeOnDevice(Expr body, SEScope se_scope, bool constrain_result, bool constrain_body) {
  if (se_scope->IsFullyUnconstrained()) {
    // Nothing to annotate with.
    return body;
  }
  if (body->IsInstance<OpNode>() || body->IsInstance<ConstructorNode>()) {
    // These operators are device polymorphic so no annotation is required.
    return body;
  }
  if (body->IsInstance<GlobalVarNode>() || body->IsInstance<VarNode>()) {
    // The device can be recovered from the binding site of the global or local variable.
    return body;
  }
  if (body->IsInstance<FunctionNode>()) {
    // If a primitive function then it is device polymorphic. Otherwise the device is captured
    // by the function's "result_se_scope" attribute.
    return body;
  }
  OnDeviceProps props = GetOnDeviceProps(body);
  if (props.body.defined()) {
    // The user is asking for
    //   on_device(on_device(body, se_scope=inner), se_scope=outer)
    //   ^         ^         ^
    //   outer     middle    inner
    // First recover the implied constraints (if any) for outer and inner, and check they don't
    // contradict.
    const SEScope& inner = props.se_scope;
    const SEScope& outer = se_scope;
    bool constrain_outer = constrain_result;
    bool constrain_inner = props.constrain_body;
    if (constrain_outer && constrain_inner) {
      ICHECK(inner == outer)
          << "Cannot constrain result and body of nested on_device calls to different SEScopes";
    }
    // There are two possible ways the middle sub-expression may be constrained, check they don't
    // contradict.
    bool constrain_middle_via_outer = constrain_body;
    bool constrain_middle_via_inner = props.constrain_result;
    if (constrain_middle_via_outer && constrain_middle_via_inner) {
      ICHECK(inner == outer)
          << "Cannot constrain intermediate result of nested on_device calls to different SEScopes";
    }
    // We can now ignore the intermediate constraints, if any.
    return OnDevice(props.body, (constrain_inner || constrain_outer) ? outer : inner,
                    constrain_outer, constrain_inner);
  } else {
    return OnDevice(body, std::move(se_scope), constrain_result, constrain_body);
  }
}

RELAY_REGISTER_OP("on_device")
    .describe(R"code(Annotate an expression with device type)code" TVM_ADD_FILELINE)
    .set_num_inputs(1)
    .add_argument("body", "Expr", "The sub-expression to be annotated.")
    .set_support_level(10)
    .add_type_rel("Identity", IdentityRel)
    .set_attrs_type_key("relay.attrs.OnDeviceAttrs")
    .set_attr<TOpPattern>("TOpPattern", kOpaque)
    .set_attr<TOpIsStateful>("TOpIsStateful", false)
    .set_attr<FInferCorrectLayout>("FInferCorrectLayout", ElemwiseArbitraryLayout)
    .set_attr<TNonComputational>("TNonComputational", true);

OnDeviceProps GetOnDeviceProps(const CallNode* call_node) {
  if (call_node->op == OnDeviceOp()) {
    ICHECK_EQ(call_node->args.size(), 1) << "on_device expects one argument";
    ICHECK(call_node->attrs.defined()) << "on_device requires attributes";
    const auto* on_device_attrs = call_node->attrs.as<OnDeviceAttrs>();
    ICHECK(on_device_attrs != nullptr) << "on_device requires OnDeviceAttrs";
    return {call_node->args[0], on_device_attrs->se_scope, on_device_attrs->constrain_result,
            on_device_attrs->constrain_body};
  }
  return {};
}

OnDeviceProps GetOnDeviceProps(const Expr& expr) {
  if (const auto* call_node = expr.as<CallNode>()) {
    return GetOnDeviceProps(call_node);
  }
  return {};
}

Function FunctionOnDevice(Function function, Array<SEScope> param_se_scopes,
                          SEScope result_se_scope) {
  return WithAttrs(std::move(function), {{tvm::attr::kParamSEScopes, std::move(param_se_scopes)},
                                         {tvm::attr::kResultSEScope, std::move(result_se_scope)}});
}

TVM_REGISTER_GLOBAL("relay.op.annotation._make.FunctionOnDevice").set_body_typed(FunctionOnDevice);

Function MaybeFunctionOnDevice(Function function, Array<SEScope> param_se_scopes,
                               SEScope result_se_scope) {
  if (std::all_of(param_se_scopes.begin(), param_se_scopes.end(),
                  [](const SEScope& se_scope) { return se_scope->IsFullyUnconstrained(); }) &&
      result_se_scope->IsFullyUnconstrained()) {
    // Nothing to annotate.
    return function;
  }
  return FunctionOnDevice(function, std::move(param_se_scopes), std::move(result_se_scope));
}

SEScope GetFunctionResultSEScope(const FunctionNode* function_node) {
  auto opt_se_scope = function_node->GetAttr<SEScope>(tvm::attr::kResultSEScope);
  return opt_se_scope.value_or(SEScope::FullyUnconstrained());
}

SEScope GetFunctionParamSEScope(const FunctionNode* function_node, size_t i) {
  ICHECK_LT(i, function_node->params.size())
      << "param index " << i << " out of range for function of arity "
      << function_node->params.size();
  auto opt_array = function_node->GetAttr<Array<SEScope>>(tvm::attr::kParamSEScopes);
  if (!opt_array) {
    // No annotation.
    return SEScope::FullyUnconstrained();
  }
  ICHECK_EQ(opt_array.value().size(), function_node->params.size())
      << "annotation parameters do not match function arity";
  return opt_array.value()[i];
}

}  // namespace relay
}  // namespace tvm
