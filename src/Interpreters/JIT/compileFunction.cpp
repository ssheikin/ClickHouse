#include "compileFunction.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>

#include <Common/Stopwatch.h>
#include <Common/ProfileEvents.h>
#include <DataTypes/Native.h>
#include <Interpreters/JIT/CHJIT.h>

namespace
{
    struct ColumnDataPlaceholder
    {
        llvm::Value * data_init; /// first row
        llvm::Value * null_init;
        llvm::PHINode * data; /// current row
        llvm::PHINode * null;
    };
}

namespace ProfileEvents
{
    extern const Event CompileFunction;
    extern const Event CompileExpressionsMicroseconds;
    extern const Event CompileExpressionsBytes;
}

namespace DB
{

ColumnData getColumnData(const IColumn * column)
{
    ColumnData result;
    const bool is_const = isColumnConst(*column);

    if (is_const)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Input columns should not be constant");

    if (const auto * nullable = typeid_cast<const ColumnNullable *>(column))
    {
        result.null = nullable->getNullMapColumn().getRawData().data;
        column = & nullable->getNestedColumn();
    }

    result.data = column->getRawData().data;

    return result;
}

static void compileFunction(llvm::Module & module, const IFunctionBaseImpl & f)
{
    ProfileEvents::increment(ProfileEvents::CompileFunction);

    const auto & arg_types = f.getArgumentTypes();

    llvm::IRBuilder<> b(module.getContext());
    auto * size_type = b.getIntNTy(sizeof(size_t) * 8);
    auto * data_type = llvm::StructType::get(b.getInt8PtrTy(), b.getInt8PtrTy());
    auto * func_type = llvm::FunctionType::get(b.getVoidTy(), { size_type, data_type->getPointerTo() }, /*isVarArg=*/false);

    auto * func = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage, f.getName(), module);
    auto * args = func->args().begin();
    llvm::Value * counter_arg = &*args++;
    llvm::Value * columns_arg = &*args++;

    auto * entry = llvm::BasicBlock::Create(b.getContext(), "entry", func);
    b.SetInsertPoint(entry);
    std::vector<ColumnDataPlaceholder> columns(arg_types.size() + 1);
    for (size_t i = 0; i <= arg_types.size(); ++i)
    {
        const auto & type = i == arg_types.size() ? f.getResultType() : arg_types[i];
        auto * data = b.CreateLoad(b.CreateConstInBoundsGEP1_32(data_type, columns_arg, i));
        columns[i].data_init = b.CreatePointerCast(b.CreateExtractValue(data, {0}), toNativeType(b, removeNullable(type))->getPointerTo());
        columns[i].null_init = type->isNullable() ? b.CreateExtractValue(data, {1}) : nullptr;
    }

    /// assume nonzero initial value in `counter_arg`
    auto * loop = llvm::BasicBlock::Create(b.getContext(), "loop", func);
    b.CreateBr(loop);
    b.SetInsertPoint(loop);
    auto * counter_phi = b.CreatePHI(counter_arg->getType(), 2);
    counter_phi->addIncoming(counter_arg, entry);
    for (auto & col : columns)
    {
        col.data = b.CreatePHI(col.data_init->getType(), 2);
        col.data->addIncoming(col.data_init, entry);
        if (col.null_init)
        {
            col.null = b.CreatePHI(col.null_init->getType(), 2);
            col.null->addIncoming(col.null_init, entry);
        }
    }

    Values arguments;
    arguments.reserve(arg_types.size());

    for (size_t i = 0; i < arg_types.size(); ++i) // NOLINT
    {
        auto & column = columns[i];
        auto type = arg_types[i];

        auto * value = b.CreateLoad(column.data);
        if (!column.null)
        {
            arguments.emplace_back(value);
            continue;
        }

        auto * is_null = b.CreateICmpNE(b.CreateLoad(column.null), b.getInt8(0));
        auto * nullable_unitilized = llvm::Constant::getNullValue(toNativeType(b, type));
        auto * nullable_value = b.CreateInsertValue(b.CreateInsertValue(nullable_unitilized, value, {0}), is_null, {1});
        arguments.emplace_back(nullable_value);
    }

    auto * result = f.compile(b, std::move(arguments));
    if (columns.back().null)
    {
        b.CreateStore(b.CreateExtractValue(result, {0}), columns.back().data);
        b.CreateStore(b.CreateSelect(b.CreateExtractValue(result, {1}), b.getInt8(1), b.getInt8(0)), columns.back().null);
    }
    else
    {
        b.CreateStore(result, columns.back().data);
    }

    auto * cur_block = b.GetInsertBlock();
    for (auto & col : columns)
    {
        col.data->addIncoming(b.CreateConstInBoundsGEP1_32(nullptr, col.data, 1), cur_block);
        if (col.null)
            col.null->addIncoming(b.CreateConstInBoundsGEP1_32(nullptr, col.null, 1), cur_block);
    }

    counter_phi->addIncoming(b.CreateSub(counter_phi, llvm::ConstantInt::get(size_type, 1)), cur_block);

    auto * end = llvm::BasicBlock::Create(b.getContext(), "end", func);
    b.CreateCondBr(b.CreateICmpNE(counter_phi, llvm::ConstantInt::get(size_type, 1)), loop, end);
    b.SetInsertPoint(end);
    b.CreateRetVoid();
}

CHJIT::CompiledModuleInfo compileFunction(CHJIT & jit, const IFunctionBaseImpl & f)
{
    Stopwatch watch;

    auto compiled_module_info = jit.compileModule([&](llvm::Module & module)
    {
        compileFunction(module, f);
    });

    ProfileEvents::increment(ProfileEvents::CompileExpressionsMicroseconds, watch.elapsedMicroseconds());
    ProfileEvents::increment(ProfileEvents::CompileExpressionsBytes, compiled_module_info.size);
    ProfileEvents::increment(ProfileEvents::CompileFunction);

    return compiled_module_info;
}

}
