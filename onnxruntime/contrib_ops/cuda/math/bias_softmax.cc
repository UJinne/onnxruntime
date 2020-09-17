// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "contrib_ops/cuda/math/bias_softmax.h"

#include "core/providers/common.h"

namespace onnxruntime {
namespace contrib {
namespace cuda {

using namespace onnxruntime::cuda;

ONNX_OPERATOR_KERNEL_EX(                                                             \
    BiasSoftmax,                                                                     \
    kMSDomain,                                                                       \
    1,                                                                               \
    kCudaExecutionProvider,                                                          \
    KernelDefBuilder().TypeConstraint("T", DataTypeImpl::AllIEEEFloatTensorTypes()), \
    BiasSoftmax);                                                              

Status BiasSoftmax::ComputeInternal(OpKernelContext* ctx) const {

  const TensorShape& X_shape{ctx->Input<Tensor>(0)->Shape()};
  const TensorShape& B_shape{ctx->Input<Tensor>(1)->Shape()};

  const Tensor* X = ctx->Input<Tensor>(0);
  const Tensor* B = ctx->Input<Tensor>(1);
  Tensor* Y = ctx->Output(0, X_shape);

  const int64_t softmax_axis = HandleNegativeAxis(softmax_axis_, X_shape.NumDimensions());
  const int N = static_cast<int>(X_shape.SizeToDimension(softmax_axis));
  const int D = static_cast<int>(X_shape.SizeFromDimension(softmax_axis));

  const int64_t broadcast_axis = HandleNegativeAxis(broadcast_axis_, X_shape.NumDimensions());
  const int broadcast_size = N/static_cast<int>(X_shape.SizeToDimension(broadcast_axis));

  const size_t elem_size = X->DataType()->Size();
  if (D <= 1024 && D*elem_size <= 4096) {
    // expect thread blocks can fill SM at high occupancy without overflowing registers
    utils::MLTypeCallDispatcher<DispatchBiasSoftmaxForward, double, float, MLFloat16> 
      t_disp(X->GetElementType());
    t_disp.Invoke(Y, X, B, D, N, D, broadcast_size);
  }
  else {
    // need to fallback to add kernel + CUDA DNN library softmax call :/
    utils::MLTypeCallDispatcher<DispatchBiasSoftMaxForwardViaDnnLibrary, double, float, MLFloat16> 
      t_disp(X->GetElementType());
    t_disp.Invoke(CudnnHandle(), D, N, broadcast_axis, softmax_axis, X_shape, X, B_shape, B, Y);
  }

  return Status::OK();
}

template <typename T>
struct DispatchBiasSoftmaxForward {
  void operator()(
    Tensor* output, 
    const Tensor* input, 
    const Tensor* input_bias, 
    int element_count, 
    int batch_count, 
    int batch_stride, 
    int bias_broadcast_size_per_batch) {
    DispatchBiasSoftmaxForwardImpl<T>(
      output, 
      input, 
      input_bias, 
      element_count, 
      batch_count, 
      batch_stride, 
      bias_broadcast_size_per_batch);
  }
};

template <typename T>
struct DispatchBiasSoftMaxForwardViaDnnLibrary {
  void operator()(
    cudnnHandle_t cudaDnnHandle,
    int element_count,
    int batch_count,
    int broadcast_axis,
    int softmax_axis,
    const onnxruntime::TensorShape& X_shape,
    const onnxruntime::Tensor* X,
    const onnxruntime::TensorShape& B_shape,
    const onnxruntime::Tensor* B,
    onnxruntime::Tensor* Y
  ) {
  DispatchBiasSoftMaxForwardViaDnnLibraryImpl<T>(
    cudaDnnHandle,
    element_count,
    batch_count,
    broadcast_axis,
    softmax_axis,
    X_shape,
    X,
    B_shape,
    B, 
    Y);
  }
};

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime