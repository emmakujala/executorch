# Copyright (c) Qualcomm Innovation Center, Inc.
# All rights reserved
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.
from typing import Dict

import executorch.backends.qualcomm.python.PyQnnWrapperAdaptor as PyQnnWrapper

import torch
from executorch.backends.qualcomm.utils.constants import QCOM_DATA

from .node_visitor import NodeVisitor
from .node_visitor_manager import register_node_visitor
from .qnn_constants import OpResizeBilinear, QNN_OP_PACKAGE_NAME_QTI_AISW


@register_node_visitor
class ResizeBilinear(NodeVisitor):
    target = ["aten.upsample_bilinear2d.default", "aten.upsample_bilinear2d.vec"]

    def __init__(self, *args) -> None:
        super().__init__(*args)

    def define_node(
        self,
        node: torch.fx.Node,
        nodes_to_wrappers: Dict[torch.fx.Node, PyQnnWrapper.TensorWrapper],
    ) -> PyQnnWrapper.PyQnnOpWrapper:
        input_node = self.get_node(node.args[0])
        input_tensor = self.get_tensor(input_node, node)
        input_tensor_wrapper = self.define_tensor(
            input_node,
            node,
            input_tensor,
            PyQnnWrapper.Qnn_TensorType_t.QNN_TENSOR_TYPE_NATIVE,
            nodes_to_wrappers,
        )

        output_tensor = self.get_tensor(node, node)
        output_tensor_wrapper = self.define_tensor(
            node,
            node,
            output_tensor,
            PyQnnWrapper.Qnn_TensorType_t.QNN_TENSOR_TYPE_NATIVE,
            nodes_to_wrappers,
        )

        resize_bilinear_op = PyQnnWrapper.PyQnnOpWrapper(
            node.name,
            QNN_OP_PACKAGE_NAME_QTI_AISW,
            OpResizeBilinear.op_name,
        )
        resize_bilinear_op.AddInputTensors([input_tensor_wrapper])
        resize_bilinear_op.AddOutputTensors([output_tensor_wrapper])

        resize_bilinear_op.AddScalarParam(
            OpResizeBilinear.param_align_corners,
            PyQnnWrapper.Qnn_DataType_t.QNN_DATATYPE_BOOL_8,
            {QCOM_DATA: node.args[2]},
        )
        resize_bilinear_op.AddScalarParam(
            OpResizeBilinear.param_half_pixel_centers,
            PyQnnWrapper.Qnn_DataType_t.QNN_DATATYPE_BOOL_8,
            {QCOM_DATA: not node.args[2]},
        )

        return resize_bilinear_op
