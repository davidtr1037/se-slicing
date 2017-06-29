#ifndef SLICEGENERATOR_H
#define SLICEGENERATOR_H

#include <stdbool.h>
#include <iostream>

#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>

#include "llvm/analysis/PointsTo/PointsTo.h"

#include "AAPass.h"
#include "ModRefAnalysis.h"
#include "Annotator.h"
#include "Cloner.h"

class SliceGenerator {
public:

    SliceGenerator(
        llvm::Module *module,
        AAPass *aa,
        ModRefAnalysis *mra,
        Annotator *annotator,
        Cloner *cloner,
        bool lazyMode = false
    ) :
        module(module), 
        aa(aa), 
        mra(mra), 
        annotator(annotator),
        cloner(cloner),
        lazyMode(lazyMode),
        llvmpta(0)
    {

    }

    void generate();

    void generateSlice(llvm::Function *f, uint32_t sliceId, ModRefAnalysis::SideEffectType type);

    void dumpSlices();

    void dumpSlice(llvm::Function *f, uint32_t sliceId, bool recursively = false);

private:

    void markAsSliced(llvm::Function *sliceEntry, uint32_t sliceId);

    void dumpSlices(ModRefAnalysis::SideEffect &sideEffect);

    llvm::Module *module;
    AAPass *aa;
    ModRefAnalysis *mra;
    Annotator *annotator;
    Cloner *cloner;
    bool lazyMode;
    dg::LLVMPointerAnalysis *llvmpta;
};

#endif
