#include <stdio.h>

#include <iostream>

#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/InstIterator.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

#include "analysis/PointsTo/PointerSubgraph.h"
#include "analysis/Offset.h"

#include "AAPass.h"
#include "SVFPointerAnalysis.h"

using namespace llvm;
using namespace dg::analysis::pta;

void SVFPointerAnalysis::run() {
    /* update virtual call related nodes */
    handleVirtualCalls();
    
    for (auto &v : pta->getNodesMap()) {
        PSNode *node = v.second.second;

        Value *node_value = node->getUserData<Value>();
        outs() << "NODE VALUE: "; node_value->print(outs()); outs() << "\n";

        switch (node->getType()) {
        case LOAD:
            handleLoad(node);
            break;
        case STORE:
            handleStore(node);
            break;
        case GEP:
            handleGep(node);
            break;
        case CAST:
            handleCast(node);
            break;
        case CONSTANT:
            break;
        case CALL_RETURN:
        case RETURN:
        case PHI:
            handlePhi(node);
            break;
        case CALL_FUNCPTR:
            break;
        case MEMCPY:
            break;
        case ALLOC:
        case DYN_ALLOC:
        case FUNCTION:
            assert(node->doesPointsTo(node, 0));
            assert(node->pointsTo.size() == 1);
        case CALL:
        case ENTRY:
        case NOOP:
            break;
        default:
            assert(0 && "Unknown type");
        }
    }
    visited.clear();
}

void SVFPointerAnalysis::handleVirtualCalls() {
    std::vector<PSNode *> funcptr_nodes;

    /* first, get all relevant nodes */
    for (auto &v : pta->getNodesMap()) {
        PSNode *node = v.second.second;
        if (node->getType() != CALL_FUNCPTR) {
            continue;
        }
        funcptr_nodes.push_back(node);
    }

    for (PSNode *node : funcptr_nodes) {
        handleFuncPtr(node);
    }
}

void SVFPointerAnalysis::handleLoad(PSNode *node) {
    PSNode *operand = node->getOperand(0);
    handleOperand(operand);
}

void SVFPointerAnalysis::handleStore(PSNode *node) {
    PSNode *operand = node->getOperand(1);
    handleOperand(operand);
}

void SVFPointerAnalysis::handleGep(PSNode *node) {
    outs() << "GEP node value:\n\t";
    node->getUserData<Value>()->print(outs());
    outs() << "\n";

    outs() << "node:\n";
    handleOperand(node);
    //PSNode *operand = node->getOperand(0);
    //outs() << "node operand:\n";
    //handleOperand(operand);
}

void SVFPointerAnalysis::handleCast(PSNode *node) {
    handleOperand(node);
    PSNode *operand = node->getOperand(0);
    handleOperand(operand);
}

void SVFPointerAnalysis::handlePhi(PSNode *node) {
    handleOperand(node);
    for (PSNode *op : node->getOperands()) {
        handleOperand(op);
    }
}

void SVFPointerAnalysis::handleFuncPtr(PSNode *node) {
    PSNode *operand = node->getOperand(0);
    handleOperand(operand);

    /* now, operand->pointsTo is updated */
    for (const Pointer& ptr : operand->pointsTo) {
        if (ptr.isValid()) {
            functionPointerCall(node, ptr.target);
        } else {
            continue;
        }
    }
}

/* based on the code from DG */
bool SVFPointerAnalysis::functionPointerCall(PSNode *callsite, PSNode *called) {
    if (!isa<Function>(called->getUserData<Value>())) {
        return false;
    }

    const Function *F = called->getUserData<Function>();
    const CallInst *CI = callsite->getUserData<CallInst>();

    if (!llvmutils::callIsCompatible(F, CI))
        return false;

    if (F->size() == 0) {
        return callsite->getPairedNode()->addPointsTo(analysis::pta::PointerUnknown);
    }

    std::pair<PSNode *, PSNode *> cf = pta->builder->createFuncptrCall(CI, F);
    assert(cf.first && cf.second);

    PSNode *ret = callsite->getPairedNode();
    ret->addOperand(cf.second);

    if (callsite->successorsNum() == 1 && callsite->getSingleSuccessor() == ret) {
        callsite->replaceSingleSuccessor(cf.first);
    } else {
        callsite->addSuccessor(cf.first);
    }

    cf.second->addSuccessor(ret);

    return true;
}

void SVFPointerAnalysis::handleOperand(PSNode *operand) {
    if (visited.find(operand) != visited.end()) {
        return;
    }
    visited.insert(operand);

    Value *value = operand->getUserData<Value>();
    if (!value) {
        return;
    }

    NodeID id = aa->getPTA()->getPAG()->getValueNode(value);
    PointsTo &pts = aa->getPTA()->getPts(id);

    outs() << "points to (" << pts.count() << "):\n";
    if (pts.empty()) {
        operand->addPointsTo(NULLPTR);
        return;
    }
    for (PointsTo::iterator i = pts.begin(); i != pts.end(); ++i) {
        NodeID node_id = *i;
        PAGNode *pagnode = aa->getPTA()->getPAG()->getPAGNode(node_id);
        if (isa<ObjPN>(pagnode)) {
            int kind = pagnode->getNodeKind();
            outs() << "obj kind: " << kind << "\n";

            if (kind == PAGNode::ObjNode || kind == PAGNode::FIObjNode) {
                /* TODO: handle FIObjNode */
                ObjPN *obj_node = dyn_cast<ObjPN>(pagnode);
                PSNode *alloc_node = getAllocNode(obj_node);

                /* add to PointsTo set */
                operand->addPointsTo(Pointer(alloc_node, 0));
            }
            if (kind == PAGNode::GepObjNode) {
                GepObjPN *gepobj_node = dyn_cast<GepObjPN>(pagnode);
                PSNode *alloc_node = getAllocNode(gepobj_node);
                uint64_t offset = getAllocNodeOffset(gepobj_node);
                outs() << "offset: " << offset << "\n";

                /* add to PointsTo set */
                operand->addPointsTo(Pointer(alloc_node, offset));
            }
        }
    }
}

PSNode *SVFPointerAnalysis::getAllocNode(ObjPN *node) {
    /* get SVF memory object (allocation site) */
    const MemObj *mem = node->getMemObj();    
    outs() << "RefVal: "; mem->getRefVal()->print(outs()); outs() << "\n";
    /* get corresponding DG node */
    PSNode *ref_node = pta->builder->getNode(mem->getRefVal());
    /* TODO: handle unexpected result */
    assert(ref_node);

    return ref_node;
}

uint64_t SVFPointerAnalysis::getAllocNodeOffset(GepObjPN *node) {
    LocationSet ls = node->getLocationSet();
    assert(ls.isConstantOffset());

    /* flat index */
    unsigned idx = ls.getOffset();
    outs() << "GepObjNode location set: " << idx << "\n";

    const MemObj *mo = node->getMemObj();
    if (!(mo->isStruct() || mo->isArray())) {
        outs() << mo->isHeap() << "\n";
        assert(false);
    }

    if (mo->isArray()) {
        /* arrays are handled insensitively */
        return UNKNOWN_OFFSET;
    }

    Type *type = mo->getTypeInfo()->getLLVMType();
    const std::vector<FieldInfo>& infovec = SymbolTableInfo::Symbolnfo()->getFlattenFieldInfoVec(type);
    assert(idx < infovec.size());

    if (infovec[idx].isPartOfArray()) {
        return UNKNOWN_OFFSET;
    }

    DataLayout *dl = SymbolTableInfo::getDataLayout();
    uint64_t offset = 0;
    for (unsigned int i = 0; i < idx; i++) {
        const Type *ftype = infovec[i].getFlattenElemTy();
        offset += dl->getTypeStoreSize((Type *)ftype);       
    }

    /* TODO: check if offset is valid */
    return offset;
}