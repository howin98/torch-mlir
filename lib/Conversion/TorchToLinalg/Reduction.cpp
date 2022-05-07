//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Also available under a BSD-style license. See LICENSE.
//
//===----------------------------------------------------------------------===//

#include "torch-mlir/Conversion/TorchToLinalg/TorchToLinalg.h"

#include "../PassDetail.h"
#include "PopulatePatterns.h"
#include "Utils.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/Matchers.h"
#include "torch-mlir/Conversion/Utils/Utils.h"
#include "torch-mlir/Dialect/Torch/IR/TorchDialect.h"
#include "torch-mlir/Dialect/Torch/IR/TorchOps.h"
#include "torch-mlir/Dialect/Torch/Utils/TorchUpstream.h"
#include "torch-mlir/Dialect/Torch/Utils/Utils.h"
#include "llvm/ADT/APSInt.h"

using namespace mlir;
using namespace mlir::torch;
using namespace mlir::torch::Torch;

namespace {
// Aten maxdim lowering represents the MaxDim op as an linalg.indexed_generic
// op, producing two output buffers.
//
// The first output buffer contains the maximum value found. It is initialized
// to the minimum representable value of the input element type.
//
// The second output buffer contains the index of the found maximum value. It is
// initialized to 0 and is resulting integer type.
//
// The indexed_generic op updates both the maximum value and index if the
// current value exceeds the running max.
class ConvertAtenMaxDimOp : public OpConversionPattern<AtenMaxDimOp> {
public:
  using OpConversionPattern<AtenMaxDimOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AtenMaxDimOp maxDimOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {

    Location loc = maxDimOp.getLoc();
    Value input = adaptor.self();
    RankedTensorType valResultType =
        getTypeConverter()
            ->convertType(maxDimOp.getResult(0).getType())
            .cast<RankedTensorType>();
    RankedTensorType idxResultType =
        getTypeConverter()
            ->convertType(maxDimOp.getResult(1).getType())
            .cast<RankedTensorType>();
    RankedTensorType inputType = input.getType().cast<RankedTensorType>();
    Type idxElementType = idxResultType.getElementType();
    if (!idxElementType.isa<IntegerType>())
      return rewriter.notifyMatchFailure(
          maxDimOp,
          "aten.max_dim to linalg.* requires integer-like result type");

    bool keepDim = false;
    if (!matchPattern(maxDimOp.keepdim(), m_TorchConstantBool(&keepDim)))
      return failure();

    int64_t dim;
    if (!matchPattern(maxDimOp.dim(), m_TorchConstantInt(&dim)))
      return rewriter.notifyMatchFailure(
          maxDimOp, "aten.max_dim to linalg.* requires int value for Dim");
    dim = toPositiveDim(dim, inputType.getRank());
    if (!isValidDim(dim, inputType.getRank()))
      return rewriter.notifyMatchFailure(maxDimOp, "dim is not a valid dim");

    Type inElementType = inputType.getElementType();
    if (!inElementType.isa<mlir::FloatType>()) {
      return rewriter.notifyMatchFailure(
          maxDimOp,
          "aten.max_dim to linalg.* requires Float input element type");
    }

    // Constant op to account for the reduction along dim.
    auto c1 = rewriter.create<arith::ConstantIndexOp>(loc, /*value=*/1);
    SmallVector<Value> resultShape;
    for (int64_t i = 0; i < inputType.getRank(); i++) {
      if (dim != i) {
        auto currentDimSize = rewriter.create<tensor::DimOp>(loc, input, i);
        resultShape.push_back(currentDimSize);
      } else if (keepDim)
        resultShape.push_back(c1);
    }
    // First fill the output buffer for the index.
    Value filledTensorIdx =
        createZeroInitTensor(rewriter, loc, resultShape, idxElementType);

    // Second fill the output buffer for the running max.
    Value initTensorMax =
        rewriter.create<linalg::InitTensorOp>(loc, resultShape, inElementType)
            .result();

    FloatAttr fillValueMaxAttr = rewriter.getFloatAttr(
        inElementType,
        APFloat::getLargest(
            inElementType.cast<mlir::FloatType>().getFloatSemantics(), true));

    Value fillValueMax =
        rewriter.create<arith::ConstantOp>(loc, fillValueMaxAttr);
    Value filledTensorMax =
        rewriter.create<linalg::FillOp>(loc, fillValueMax, initTensorMax)
            .result();

    // Create the affine expressions that will be used to
    // iterate over the input and output tensors.
    // Here we also set the type of iterator: parallel or reduction.
    SmallVector<AffineExpr> exprs;
    SmallVector<StringRef> iteratorTypes;
    SmallVector<AffineExpr> resultExprs;
    for (auto size : llvm::enumerate(inputType.getShape())) {
      exprs.push_back(rewriter.getAffineDimExpr(size.index()));

      if (unsigned(dim) == size.index()) {
        iteratorTypes.push_back(getReductionIteratorTypeName());
        // If `keepDim`, create affine map to the first element
        // in the current dimension.
        if (keepDim)
          resultExprs.push_back(rewriter.getAffineConstantExpr(0));
      } else {
        iteratorTypes.push_back(getParallelIteratorTypeName());
        resultExprs.push_back(rewriter.getAffineDimExpr(size.index()));
      }
    }
    auto maps = AffineMap::inferFromExprList({exprs, resultExprs, resultExprs});
    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc,
        ArrayRef<Type>({filledTensorMax.getType(), filledTensorIdx.getType()}),
        input, ValueRange({filledTensorMax, filledTensorIdx}), maps,
        iteratorTypes,
        [&](OpBuilder &nestedBuilder, Location nestedLoc,
            ValueRange blockArgs) {
          Value newValue = blockArgs[0];
          Value oldValue = blockArgs[1];
          Value oldIndex = blockArgs[2];

          Value newIndex = rewriter.create<arith::IndexCastOp>(
              nestedLoc, oldIndex.getType(),
              rewriter.create<linalg::IndexOp>(loc, dim));

          Value predicate;
          if (inElementType.isa<mlir::FloatType>())
            predicate = rewriter.create<arith::CmpFOp>(
                nestedLoc, arith::CmpFPredicate::OGT, newValue, oldValue);
          auto resultMax = rewriter.create<arith::SelectOp>(
              nestedLoc, predicate, newValue, oldValue);
          auto resultIndex = rewriter.create<arith::SelectOp>(
              nestedLoc, predicate, newIndex, oldIndex);
          nestedBuilder.create<linalg::YieldOp>(
              nestedLoc, ValueRange({resultMax, resultIndex}));
        });

    // This cast is required to fix the shape in the case of keepDim=True
    Value maxValuesCast = rewriter.create<tensor::CastOp>(
        loc, valResultType, linalgOp.getResult(0));
    Value maxIdxCast = rewriter.create<tensor::CastOp>(loc, idxResultType,
                                                       linalgOp.getResult(1));
    rewriter.replaceOp(maxDimOp, {maxValuesCast, maxIdxCast});
    return success();
  }
};
} // namespace

static Value createLinalgNeutralElementForReduceOp(OpBuilder &b, Location loc,
                                                   Operation *op,
                                                   Type elementType) {
  if (isa<AtenSumOp, AtenSumDimIntListOp>(op))
    return b.create<arith::ConstantOp>(loc, b.getZeroAttr(elementType));

  if (isa<AtenMaxOp>(op)) {
    if (elementType.isa<mlir::FloatType>())
      return b.create<arith::ConstantOp>(
          loc, b.getFloatAttr(
                   elementType,
                   APFloat::getLargest(
                       elementType.cast<mlir::FloatType>().getFloatSemantics(),
                       /*Negative=*/true)));
    else if (elementType.isa<mlir::IntegerType>() &&
             elementType.getIntOrFloatBitWidth() != 8)
      return b.create<arith::ConstantOp>(
          loc, b.getIntegerAttr(elementType,
                                APSInt::getSignedMinValue(
                                    elementType.getIntOrFloatBitWidth())));
  }

  op->emitError("unimplemented lowering in "
                "createLinalgNeutralElementForReduceOp");
  return nullptr;
}

static Value createLinalgPayloadCalculationForReduceOp(OpBuilder &b,
                                                       Location loc,
                                                       ValueRange payloadArgs,
                                                       Operation *op,
                                                       Type resultElementType) {
  if (isa<AtenSumOp, AtenSumDimIntListOp>(op)) {
    Value self =
        convertScalarToDtype(b, loc, payloadArgs[0], resultElementType);
    Value result = payloadArgs[1];
    if (resultElementType.isa<mlir::FloatType>())
      return b.create<arith::AddFOp>(loc, self, result);
    else if (resultElementType.isa<mlir::IntegerType>())
      return b.create<arith::AddIOp>(loc, self, result);
  } else if (auto max = dyn_cast<AtenMaxOp>(op)) {
    Value self =
        convertScalarToDtype(b, loc, payloadArgs[0], resultElementType);
    Value result = payloadArgs[1];
    if (resultElementType.isa<mlir::FloatType>())
      return b.create<arith::MaxFOp>(loc, self, result);
    else if (resultElementType.isa<mlir::IntegerType>()) {
      IntegerType intType = max.self()
                                .getType()
                                .cast<BaseTensorType>()
                                .getDtype()
                                .dyn_cast<mlir::IntegerType>();
      if (intType.isUnsigned())
        return b.create<arith::MaxUIOp>(loc, self, result);
      if (intType.isSigned())
        return b.create<arith::MaxSIOp>(loc, self, result);
    }
  }
  op->emitError("unimplemented lowering in "
                "createLinalgPayloadCalculationForReduceOp");
  return nullptr;
}

namespace {
class ConvertReductionOp : public ConversionPattern {
private:
  struct ReductionOpInfo {
    bool keepDim;
    Value tensorOperand;
    DenseSet<int64_t> dimSet;
  };

  /// Given a reduction operation that has the `keepdim` attribute and the
  /// (optional) `dim` attribute, extract the source tensor operand and the
  /// literal values of the attributes into a value of type `ReductionOpInfo`
  /// into `opInfo`. The `opInfo` type is invalid if this method returns
  /// failure.
  template <typename T>
  LogicalResult
  computeReductionOpInfoFromDimOp(T op, ArrayRef<Value> operands,
                                  ConversionPatternRewriter &rewriter,
                                  ReductionOpInfo &opInfo) const {
    opInfo.tensorOperand = operands[0];
    auto inputType = opInfo.tensorOperand.getType().cast<RankedTensorType>();

    if (!matchPattern(op.keepdim(), m_TorchConstantBool(&opInfo.keepDim)))
      return rewriter.notifyMatchFailure(op, "keepdim not present");

    SmallVector<int64_t> dimList;
    if (matchPattern(op.dim(), m_TorchConstantIntList(dimList))) {
      // Fix negative dimensions, if any, before adding to the list.
      for (auto dim : dimList) {
        dim = dim >= 0 ? dim : dim + inputType.getRank();
        // Drop invalid dimensions
        if (dim < inputType.getRank())
          opInfo.dimSet.insert(dim);
      }
    } else {
      // Reduce along all dimensions
      for (int64_t i = 0; i < inputType.getRank(); i++)
        opInfo.dimSet.insert(i);
    }

    return success();
  }

  /// Given a reduction operation, extract the source tensor operand and the
  /// literal values of the `keepdim` and `dim` attributes, if any, into a value
  /// of type `ReductionOpInfo` into `opInfo`.  The `opInfo` type is invalid if
  /// this method returns failure.
  LogicalResult computeReductionOpInfo(Operation *op, ArrayRef<Value> operands,
                                       ConversionPatternRewriter &rewriter,
                                       ReductionOpInfo &opInfo) const {
    opInfo.keepDim = false;

    if (isa<AtenMaxOp, AtenSumOp>(op)) {
      opInfo.tensorOperand = operands[0];
      auto inputType = opInfo.tensorOperand.getType().cast<RankedTensorType>();

      // `AtenSumOp` and `AtenMaxOp` reduces along all the dimensions of the
      // input tensor.
      for (int64_t i = 0; i < inputType.getRank(); i++)
        opInfo.dimSet.insert(i);

      return success();
    }

    if (auto sumOp = dyn_cast<AtenSumDimIntListOp>(op))
      return computeReductionOpInfoFromDimOp(sumOp, operands, rewriter, opInfo);

    if (auto normOp = dyn_cast<AtenLinalgVectorNormOp>(op))
      return computeReductionOpInfoFromDimOp(normOp, operands, rewriter,
                                             opInfo);

    return rewriter.notifyMatchFailure(op, "not a supported reduce op");
  }

  Value payloadForNormSummation(OpBuilder &b, Location loc,
                                ValueRange payloadArgs, Value ord,
                                Type elemType) const {
    // TODO: Short-circuit operations if `ord` is zero or one.
    auto elem = payloadArgs[0];
    auto result = payloadArgs[1];
    auto self = convertScalarToDtype(b, loc, elem, elemType);
    auto abs = b.create<math::AbsOp>(loc, self);
    auto pow = b.create<math::PowFOp>(loc, abs, ord);
    return b.create<arith::AddFOp>(loc, pow, result);
  }

  /// Generate a linalg.generic operation for performing a sum reduction along
  /// the tensor and dimensions specified in `opInfo`, such that the element
  /// type of the result tensor is `elemType`.
  Value createNormSumReduction(Location loc, Type elemType,
                               const ReductionOpInfo &opInfo, Value ord,
                               ConversionPatternRewriter &rewriter) const {
    auto err = false;

    // Function to create the body of the linalg.generic operation.
    auto sumBodyBuilder = [&](OpBuilder &builder, Location loc,
                              ValueRange payloadArgs) {
      auto result =
          payloadForNormSummation(builder, loc, payloadArgs, ord, elemType);
      if (result)
        builder.create<linalg::YieldOp>(loc, result);
      err = !result;
    };

    auto zeroAttr = rewriter.getZeroAttr(elemType);
    auto initElement = rewriter.create<arith::ConstantOp>(loc, zeroAttr);

    // linalg.generic operation for sum reduction
    auto sumOp = torch_to_linalg::createReductionLinalgGeneric(
        rewriter, loc, opInfo.tensorOperand, opInfo.dimSet, opInfo.keepDim,
        initElement, sumBodyBuilder);
    return err || !sumOp ? Value{} : sumOp;
  }

  /// Generate a linalg.generic operation for pointwise exponentiation of each
  /// element.
  Value createNormExp(Location loc, Type elemType, Value exponent, Value sumOp,
                      const ReductionOpInfo &opInfo,
                      ConversionPatternRewriter &rewriter) const {
    bool err = false;
    auto powBodyBuilder = [&](OpBuilder &builder, Location loc,
                              ValueRange payloadArgs) {
      auto elem = convertScalarToDtype(builder, loc, payloadArgs[0], elemType);
      auto result = builder.create<math::PowFOp>(loc, elem, exponent);
      if (result)
        builder.create<linalg::YieldOp>(loc, Value{result});
      err = !result;
    };

    auto powOp = torch_to_linalg::createElementwiseLinalgGeneric(
        rewriter, loc, {sumOp}, elemType, powBodyBuilder);
    return err || !powOp ? Value{} : powOp;
  }

public:
  ConvertReductionOp(TypeConverter &typeConverter, MLIRContext *context)
      : ConversionPattern(typeConverter, MatchAnyOpTypeTag(), /*benefit=*/1,
                          context) {}
  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    if (failed(verifyLinalgCompatibleTypes(op, rewriter)))
      return failure();

    auto opInfo = ReductionOpInfo{false, Value{}, {}};
    if (failed(computeReductionOpInfo(op, operands, rewriter, opInfo)))
      return failure();

    Location loc = op->getLoc();
    auto resultType = getTypeConverter()
                          ->convertType(op->getResult(0).getType())
                          .cast<RankedTensorType>();

    if (auto normOp = dyn_cast<AtenLinalgVectorNormOp>(op)) {
      auto elemType = resultType.getElementType();

      if (!elemType.isa<mlir::FloatType>())
        return rewriter.notifyMatchFailure(
            op, "only float types are valid for vector norm ops");

      // Cast `ord` to float so that we can readily pass it math.powf.
      auto ordValue = operands[1];
      if (ordValue.getType().isa<mlir::IntegerType>())
        ordValue = rewriter.create<arith::SIToFPOp>(loc, elemType, ordValue);

      // Sum each element of the tensor after computing the exponentiation.
      auto sumOp =
          createNormSumReduction(loc, elemType, opInfo, ordValue, rewriter);
      if (!sumOp)
        return failure();

      // TODO: Add support for L0 norm.
      auto epsilon = 1e-5;
      auto ordLiteral = 0.0;
      if (matchPattern(ordValue, m_TorchConstantFloat(&ordLiteral)) &&
          fabs(ordLiteral) < epsilon)
        return rewriter.notifyMatchFailure(op, "unimplemented: L0 norm");

      // Raise each summed value to the inverse of the order of the norm.
      auto oneAttr = rewriter.getFloatAttr(elemType, 1.0);
      auto oneValue = rewriter.create<arith::ConstantOp>(loc, oneAttr);
      auto inverseOrdValue =
          rewriter.create<arith::DivFOp>(loc, oneValue, ordValue);
      auto expOp = createNormExp(loc, elemType, inverseOrdValue, sumOp, opInfo,
                                 rewriter);
      if (!expOp)
        return failure();

      rewriter.replaceOpWithNewOp<tensor::CastOp>(op, resultType, expOp);
      return success();
    }

    Value initElem = createLinalgNeutralElementForReduceOp(
        rewriter, loc, op, resultType.getElementType());

    bool hadErrorCreatingPayload = false;
    Value generic = torch_to_linalg::createReductionLinalgGeneric(
        rewriter, loc, opInfo.tensorOperand, opInfo.dimSet, opInfo.keepDim,
        initElem, [&](OpBuilder &b, Location loc, ValueRange payloadArgs) {
          Value result = createLinalgPayloadCalculationForReduceOp(
              b, loc, payloadArgs, op, resultType.getElementType());
          if (!result) {
            hadErrorCreatingPayload = true;
            return;
          }
          b.create<linalg::YieldOp>(loc, result);
        });

    if (hadErrorCreatingPayload)
      return failure();
    rewriter.replaceOpWithNewOp<tensor::CastOp>(op, resultType, generic);
    return success();
  }
};
} // namespace

void mlir::torch::torch_to_linalg::populateReductionPatternsAndLegality(
    TypeConverter &typeConverter, RewritePatternSet &patterns,
    ConversionTarget &target) {
  MLIRContext *context = patterns.getContext();
  target.addIllegalOp<AtenMaxDimOp>();
  patterns.add<ConvertAtenMaxDimOp>(typeConverter, context);
  target.addIllegalOp<AtenSumOp>();
  target.addIllegalOp<AtenSumDimIntListOp>();
  target.addIllegalOp<AtenMaxOp>();
  target.addIllegalOp<AtenLinalgVectorNormOp>();
  patterns.add<ConvertReductionOp>(typeConverter, context);
}
