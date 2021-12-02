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
 * \file src/relay/op/contrib/ethosu/identity.cc
 * \brief Property def of the Arm(R) Ethos(TM)-U NPU identity op.
 */
#include <tvm/relay/op.h>

#include "common.h"

namespace tvm {
namespace relay {
namespace op {
namespace contrib {
namespace ethosu {

/*! \brief Attributes used by the NPU identity operator */
struct EthosuIdentityAttrs : public tvm::AttrsNode<EthosuIdentityAttrs> {
  double ifm_scale;
  int ifm_zero_point;
  double ofm_scale;
  int ofm_zero_point;
  String activation;

  TVM_DECLARE_ATTRS(EthosuIdentityAttrs, "relay.attrs.EthosuIdentityAttrs") {
    TVM_ATTR_FIELD(ifm_scale).describe("The quantization scale for the Input Feature Map tensor.");
    TVM_ATTR_FIELD(ifm_zero_point)
        .describe("The quantization zero point for the Input Feature Map tensor.");
    TVM_ATTR_FIELD(ofm_scale).describe("The quantization scale for the Output Feature Map tensor.");
    TVM_ATTR_FIELD(ofm_zero_point)
        .describe("The quantization zero point for the Output Feature Map tensor.");
    TVM_ATTR_FIELD(activation)
        .describe(
            "The activation function to use. "
            "'NONE' - no activation function. "
            "'TANH' - tanh activation function. "
            "'SIGMOID' - sigmoid activation function. "
            "'LUT' - use a look-up table to perform the activation function.")
        .set_default("NONE");
  }
};

TVM_REGISTER_NODE_TYPE(EthosuIdentityAttrs);

bool EthosuIdentityRel(const Array<Type>& types, int num_inputs, const Attrs& attrs,
                       const TypeReporter& reporter) {
  const int ifm_index = 0;
  const int result_index = 2;
  ICHECK_EQ(types.size(), result_index + 1);

  const auto* ifm = types[ifm_index].as<TensorTypeNode>();
  if (ifm == nullptr) return false;

  const auto* param = attrs.as<EthosuIdentityAttrs>();

  ICHECK(param != nullptr) << "EthosuIdentityAttrs cannot be nullptr.";

  if (ifm->dtype != DataType::UInt(8) && ifm->dtype != DataType::Int(8)) {
    reporter->GetDiagCtx().EmitFatal(
        Diagnostic::Error(reporter->GetSpan())
        << "Invalid operator: Expected type(uint8) or type(int8) for ifm but was " << ifm->dtype);
    return false;
  }

  if (ifm->shape.size() > 4) {
    reporter->GetDiagCtx().EmitFatal(
        Diagnostic::Error(reporter->GetSpan())
        << "Invalid operator: Input Feature Map should be at most 4 dimensional, but was "
        << ifm->shape);
    return false;
  }

  // Assign ofm type
  auto ofm_shape = ifm->shape;
  reporter->Assign(types[result_index], TensorType(ofm_shape, ifm->dtype));
  return true;
}

Expr MakeEthosuIdentity(Expr ifm, Expr lut, double ifm_scale, int ifm_zero_point, double ofm_scale,
                        int ofm_zero_point, String activation) {
  auto attrs = make_object<EthosuIdentityAttrs>();
  attrs->ifm_scale = ifm_scale;
  attrs->ifm_zero_point = ifm_zero_point;
  attrs->ofm_scale = ofm_scale;
  attrs->ofm_zero_point = ofm_zero_point;
  attrs->activation = std::move(activation);
  static const Op& op = Op::Get("contrib.ethosu.identity");
  return Call(op, {ifm, lut}, Attrs(attrs), {});
}

TVM_REGISTER_GLOBAL("relay.op._make.ethosu_identity").set_body_typed(MakeEthosuIdentity);

RELAY_REGISTER_OP("contrib.ethosu.identity")
    .describe(R"code(Arm(R) Ethos(TM)-U NPU identity operator.

This Relay operator performs the identity pooling operation on the NPU with a capability
to requantize the data. It accepts input tensors of 4 dimensions or less.

)code" TVM_ADD_FILELINE)
    .set_attrs_type<EthosuIdentityAttrs>()
    .set_num_inputs(2)
    .add_argument("ifm", "Tensor", "The Input Feature Map tensor (IFM).")
    .add_argument("lut", "Tensor", "The look-up table values to use if activation = 'LUT'.")
    .set_support_level(11)
    .add_type_rel("EthosuIdentity", EthosuIdentityRel);

}  // namespace ethosu
}  // namespace contrib
}  // namespace op
}  // namespace relay
}  // namespace tvm
