/*
 * Copyright (C) 2016-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebAssemblyFunction.h"

#if ENABLE(WEBASSEMBLY)

#include "JSCJSValueInlines.h"
#include "JSObject.h"
#include "JSObjectInlines.h"
#include "JSToWasm.h"
#include "JSWebAssemblyHelpers.h"
#include "JSWebAssemblyInstance.h"
#include "JSWebAssemblyMemory.h"
#include "JSWebAssemblyRuntimeError.h"
#include "LLIntThunks.h"
#include "LinkBuffer.h"
#include "ProtoCallFrameInlines.h"
#include "SlotVisitorInlines.h"
#include "StructureInlines.h"
#include "WasmCallee.h"
#include "WasmCallingConvention.h"
#include "WasmContextInlines.h"
#include "WasmFormat.h"
#include "WasmMemory.h"
#include "WasmMemoryInformation.h"
#include "WasmModuleInformation.h"
#include "WasmTypeDefinitionInlines.h"
#include <wtf/StackPointer.h>
#include <wtf/SystemTracing.h>

namespace JSC {

const ClassInfo WebAssemblyFunction::s_info = { "WebAssemblyFunction"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(WebAssemblyFunction) };

static JSC_DECLARE_HOST_FUNCTION(callWebAssemblyFunction);

JSC_DEFINE_HOST_FUNCTION(callWebAssemblyFunction, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    WebAssemblyFunction* wasmFunction = jsCast<WebAssemblyFunction*>(callFrame->jsCallee());
    Wasm::TypeIndex typeIndex = wasmFunction->typeIndex();
    const Wasm::FunctionSignature& signature = Wasm::TypeInformation::getFunctionSignature(typeIndex);

    // Make sure that the memory we think we are going to run with matches the one we expect.
    ASSERT(wasmFunction->instance()->instance().calleeGroup()->isSafeToRun(wasmFunction->instance()->memory()->memory().mode()));

    std::optional<TraceScope> traceScope;
    if (Options::useTracePoints())
        traceScope.emplace(WebAssemblyExecuteStart, WebAssemblyExecuteEnd);

    Vector<JSValue, MarkedArgumentBuffer::inlineCapacity> boxedArgs;
    JSWebAssemblyInstance* instance = wasmFunction->instance();
    Wasm::Instance* wasmInstance = &instance->instance();

    for (unsigned argIndex = 0; argIndex < signature.argumentCount(); ++argIndex) {
        if (signature.argumentType(argIndex).isV128())
            return JSValue::encode(throwException(globalObject, scope, createTypeError(globalObject, Wasm::errorMessageForExceptionType(Wasm::ExceptionType::TypeErrorInvalidV128Use))));
        uint64_t value = fromJSValue(globalObject, signature.argumentType(argIndex), callFrame->argument(argIndex));
        RETURN_IF_EXCEPTION(scope, encodedJSValue());
        boxedArgs.append(JSValue::decode(value));
    }

    // When we don't use fast TLS to store the context, the JS
    // entry wrapper expects a JSWebAssemblyInstance as the |this| value argument.
    JSValue context = Wasm::Context::useFastTLS() ? JSValue() : instance;
    JSValue* args = boxedArgs.data();
    int argCount = boxedArgs.size() + 1;

    // Note: we specifically use the WebAssemblyFunction as the callee to begin with in the ProtoCallFrame.
    // The reason for this is that calling into the llint may stack overflow, and the stack overflow
    // handler might read the global object from the callee.
    ProtoCallFrame protoCallFrame;
    protoCallFrame.init(nullptr, globalObject, wasmFunction, context, argCount, args);

    // FIXME Do away with this entire function, and only use the entrypoint generated by B3. https://bugs.webkit.org/show_bug.cgi?id=166486
    Wasm::Instance* prevWasmInstance = vm.wasmContext.load();
    {
        // We do the stack check here for the wrapper function because we don't
        // want to emit a stack check inside every wrapper function.
        const uintptr_t sp = bitwise_cast<uintptr_t>(currentStackPointer());
        const uintptr_t frameSize = (boxedArgs.size() + CallFrame::headerSizeInRegisters) * sizeof(Register);
        const uintptr_t stackSpaceUsed = 2 * frameSize; // We're making two calls. One to the wrapper, and one to the actual wasm code.
        if (UNLIKELY((sp < stackSpaceUsed) || ((sp - stackSpaceUsed) < bitwise_cast<uintptr_t>(vm.softStackLimit()))))
            return JSValue::encode(throwException(globalObject, scope, createStackOverflowError(globalObject)));
    }
    vm.wasmContext.store(wasmInstance, vm.softStackLimit());
    ASSERT(wasmFunction->instance());
    ASSERT(&wasmFunction->instance()->instance() == vm.wasmContext.load());
    EncodedJSValue rawResult = vmEntryToWasm(wasmFunction->jsEntrypoint(MustCheckArity).taggedPtr(), &vm, &protoCallFrame);
    if (prevWasmInstance != wasmInstance) {
        // This is just for some extra safety instead of leaving a cached
        // value in there. If we ever forget to set the value to be a real
        // bounds, this will force every stack overflow check to immediately
        // fire. The stack limit never changes while executing except when
        // WebAssembly is used through the JSC API: API users can ask the code
        // to migrate threads.
        wasmInstance->setCachedStackLimit(bitwise_cast<void*>(std::numeric_limits<uintptr_t>::max()));
    }
    vm.wasmContext.store(prevWasmInstance, vm.softStackLimit());
    RETURN_IF_EXCEPTION(scope, { });

    // We need to make sure this is in a register or on the stack since it's stored in Vector<JSValue>.
    // This probably isn't strictly necessary, since the WebAssemblyFunction* should keep the instance
    // alive. But it's good hygiene.
    instance->use();

    return rawResult;
}

bool WebAssemblyFunction::usesTagRegisters() const
{
    const auto& signature = Wasm::TypeInformation::getFunctionSignature(typeIndex());
    return signature.argumentCount() || !signature.returnsVoid();
}

RegisterSet WebAssemblyFunction::calleeSaves() const
{
    // Pessimistically save callee saves in BoundsChecking mode since the LLInt always bounds checks
    RegisterSetBuilder result = Wasm::PinnedRegisterInfo::get().toSave(MemoryMode::BoundsChecking);
    if (usesTagRegisters()) {
        RegisterSetBuilder tagCalleeSaves = RegisterSetBuilder::calleeSaveRegisters();
        tagCalleeSaves.filter(RegisterSetBuilder::runtimeTagRegisters());
        result.merge(tagCalleeSaves);
    }
    return result.buildAndValidate();
}

RegisterAtOffsetList WebAssemblyFunction::usedCalleeSaveRegisters() const
{
    return RegisterAtOffsetList { calleeSaves(), RegisterAtOffsetList::OffsetBaseType::FramePointerBased };
}

ptrdiff_t WebAssemblyFunction::previousInstanceOffset() const
{
    ptrdiff_t result = calleeSaves().numberOfSetRegisters() * sizeof(CPURegister);
    result = -result - sizeof(CPURegister);
#if USE(JSVALUE32_64)
    ASSERT(!calleeSaves().numberOfSetFPRs()); // Because FPRs are wider than sizeof(CPURegister)
#endif
#if ASSERT_ENABLED
    ptrdiff_t minOffset = 1;
    for (const RegisterAtOffset& regAtOffset : usedCalleeSaveRegisters()) {
        ptrdiff_t offset = regAtOffset.offset();
        ASSERT(offset < 0);
        minOffset = std::min(offset, minOffset);
    }
    ASSERT(minOffset - static_cast<ptrdiff_t>(sizeof(CPURegister)) == result);
#endif
    return result;
}

Wasm::Instance* WebAssemblyFunction::previousInstance(CallFrame* callFrame)
{
    ASSERT(callFrame->callee().rawPtr() == m_jsToWasmICCallee.get());
    auto* result = *bitwise_cast<Wasm::Instance**>(bitwise_cast<char*>(callFrame) + previousInstanceOffset());
    return result;
}

CodePtr<JSEntryPtrTag> WebAssemblyFunction::jsCallEntrypointSlow()
{
    if (Options::forceICFailure())
        return nullptr;

    VM& vm = this->vm();
    CCallHelpers jit;

    JIT_COMMENT(jit, "jsCallEntrypointSlow");

    const auto& typeDefinition = Wasm::TypeInformation::get(typeIndex()).expand();
    const auto& signature = *typeDefinition.as<Wasm::FunctionSignature>();
    const auto& pinnedRegs = Wasm::PinnedRegisterInfo::get();
    RegisterAtOffsetList registersToSpill = usedCalleeSaveRegisters();

    const Wasm::WasmCallingConvention& wasmCC = Wasm::wasmCallingConvention();
    Wasm::CallInformation wasmCallInfo = wasmCC.callInformationFor(typeDefinition);
    if (wasmCallInfo.argumentsOrResultsIncludeV128)
        return nullptr;
    Wasm::CallInformation jsCallInfo = Wasm::jsCallingConvention().callInformationFor(typeDefinition, Wasm::CallRole::Callee);
    RegisterAtOffsetList savedResultRegisters = wasmCallInfo.computeResultsOffsetList();

    unsigned totalFrameSize = registersToSpill.sizeOfAreaInBytes();
    totalFrameSize += sizeof(CPURegister); // Slot for the VM's previous wasm instance.
    totalFrameSize += wasmCallInfo.headerAndArgumentStackSizeInBytes;
    totalFrameSize += savedResultRegisters.sizeOfAreaInBytes();

    // FIXME: Optimize Wasm function call even if arguments include I64.
    // This requires I64 extraction from BigInt.
    // https://bugs.webkit.org/show_bug.cgi?id=220053
    if (wasmCallInfo.argumentsIncludeI64)
        return nullptr;

    totalFrameSize = WTF::roundUpToMultipleOf(stackAlignmentBytes(), totalFrameSize);

    jit.emitFunctionPrologue();
    jit.subPtr(MacroAssembler::TrustedImm32(totalFrameSize), MacroAssembler::stackPointerRegister);
    jit.emitZeroToCallFrameHeader(CallFrameSlot::codeBlock);

    for (const RegisterAtOffset& regAtOffset : registersToSpill) {
        GPRReg reg = regAtOffset.reg().gpr();
        ptrdiff_t offset = regAtOffset.offset();
        jit.storePtr(reg, CCallHelpers::Address(GPRInfo::callFrameRegister, offset));
    }

    JSValueRegs scratchJSR {
#if USE(JSVALUE32_64)
        Wasm::wasmCallingConvention().prologueScratchGPRs[2],
#endif
        Wasm::wasmCallingConvention().prologueScratchGPRs[1]
    };
    GPRReg stackLimitGPR = Wasm::wasmCallingConvention().prologueScratchGPRs[0];
    bool stackLimitGPRIsClobbered = false;
    jit.loadPtr(vm.addressOfSoftStackLimit(), stackLimitGPR);

    CCallHelpers::JumpList slowPath;
    slowPath.append(jit.branchPtr(CCallHelpers::Above, MacroAssembler::stackPointerRegister, GPRInfo::callFrameRegister));
    slowPath.append(jit.branchPtr(CCallHelpers::Below, MacroAssembler::stackPointerRegister, stackLimitGPR));

    // Ensure:
    // argCountPlusThis - 1 >= signature.argumentCount()
    // argCountPlusThis >= signature.argumentCount() + 1
    // FIXME: We should handle mismatched arity
    // https://bugs.webkit.org/show_bug.cgi?id=196564
    slowPath.append(jit.branch32(CCallHelpers::Below,
        CCallHelpers::payloadFor(CallFrameSlot::argumentCountIncludingThis), CCallHelpers::TrustedImm32(signature.argumentCount() + 1)));

    if (usesTagRegisters())
        jit.emitMaterializeTagCheckRegisters();

    // Loop backwards so we can use the first floating point argument as a scratch.
    FPRReg scratchFPR = Wasm::wasmCallingConvention().fprArgs[0];
    for (unsigned i = signature.argumentCount(); i--;) {
        CCallHelpers::Address calleeFrame = CCallHelpers::Address(MacroAssembler::stackPointerRegister, 0);
        CCallHelpers::Address jsParam(GPRInfo::callFrameRegister, jsCallInfo.params[i].location.offsetFromFP());
        bool isStack = wasmCallInfo.params[i].location.isStackArgument();

        auto type = signature.argumentType(i);
        switch (type.kind) {
        case Wasm::TypeKind::I32: {
            jit.loadValue(jsParam, scratchJSR);
            slowPath.append(jit.branchIfNotInt32(scratchJSR));
            if (isStack) {
                CCallHelpers::Address addr { calleeFrame.withOffset(wasmCallInfo.params[i].location.offsetFromSP()) };
                jit.store32(scratchJSR.payloadGPR(), addr.withOffset(PayloadOffset));
#if USE(JSVALUE32_64)
                jit.store32(CCallHelpers::TrustedImm32(0), addr.withOffset(TagOffset));
#endif
            } else {
                jit.zeroExtend32ToWord(scratchJSR.payloadGPR(), wasmCallInfo.params[i].location.jsr().payloadGPR());
#if USE(JSVALUE32_64)
                jit.move(CCallHelpers::TrustedImm32(0), wasmCallInfo.params[i].location.jsr().tagGPR());
#endif
            }
            break;
        }
        case Wasm::TypeKind::Ref:
        case Wasm::TypeKind::RefNull:
        case Wasm::TypeKind::Funcref:
        case Wasm::TypeKind::Externref: {
            if (!Wasm::isExternref(type)) {
                // Ensure we have a WASM exported function.
                jit.loadValue(jsParam, scratchJSR);
                auto isNull = jit.branchIfNull(scratchJSR);
                if (!type.isNullable())
                    slowPath.append(isNull);
                slowPath.append(jit.branchIfNotCell(scratchJSR));

                stackLimitGPRIsClobbered = true;
                jit.emitLoadStructure(vm, scratchJSR.payloadGPR(), scratchJSR.payloadGPR());
                jit.loadCompactPtr(CCallHelpers::Address(scratchJSR.payloadGPR(), Structure::classInfoOffset()), scratchJSR.payloadGPR());

                static_assert(std::is_final<WebAssemblyFunction>::value, "We do not check for subtypes below");
                static_assert(std::is_final<WebAssemblyWrapperFunction>::value, "We do not check for subtypes below");

                auto isWasmFunction = jit.branchPtr(CCallHelpers::Equal, scratchJSR.payloadGPR(), CCallHelpers::TrustedImmPtr(WebAssemblyFunction::info()));
                slowPath.append(jit.branchPtr(CCallHelpers::NotEqual, scratchJSR.payloadGPR(), CCallHelpers::TrustedImmPtr(WebAssemblyWrapperFunction::info())));

                isWasmFunction.link(&jit);
                if (Wasm::isRefWithTypeIndex(type)) {
                    jit.loadPtr(jsParam, scratchJSR.payloadGPR());
                    jit.loadPtr(CCallHelpers::Address(scratchJSR.payloadGPR(), WebAssemblyFunctionBase::offsetOfSignatureIndex()), scratchJSR.payloadGPR());
                    slowPath.append(jit.branchPtr(CCallHelpers::NotEqual, scratchJSR.payloadGPR(), CCallHelpers::TrustedImmPtr(type.index)));
                }

                if (type.isNullable())
                    isNull.link(&jit);
            }

            if (isStack) {
                jit.loadValue(jsParam, scratchJSR);
                if (!type.isNullable())
                    slowPath.append(jit.branchIfNull(scratchJSR));
                jit.storeValue(scratchJSR, calleeFrame.withOffset(wasmCallInfo.params[i].location.offsetFromSP()));
            } else {
                auto externJSR = wasmCallInfo.params[i].location.jsr();
                jit.loadValue(jsParam, externJSR);
                if (!type.isNullable())
                    slowPath.append(jit.branchIfNull(externJSR));
            }
            break;
        }
        case Wasm::TypeKind::F32:
        case Wasm::TypeKind::F64: {
            if (!isStack)
                scratchFPR = wasmCallInfo.params[i].location.fpr();

            jit.loadValue(jsParam, scratchJSR);
#if USE(JSVALUE64)
            slowPath.append(jit.branchIfNotNumber(scratchJSR, InvalidGPRReg));
#elif USE(JSVALUE32_64)
            stackLimitGPRIsClobbered = true;
            slowPath.append(jit.branchIfNotNumber(scratchJSR, stackLimitGPR));
#endif
            auto isInt32 = jit.branchIfInt32(scratchJSR);
#if USE(JSVALUE64)
            jit.unboxDouble(scratchJSR.payloadGPR(), scratchJSR.payloadGPR(), scratchFPR);
#elif USE(JSVALUE32_64)
            jit.unboxDouble(scratchJSR, scratchFPR);
#endif
            if (signature.argumentType(i).isF32())
                jit.convertDoubleToFloat(scratchFPR, scratchFPR);
            auto done = jit.jump();

            isInt32.link(&jit);
            if (signature.argumentType(i).isF32()) {
                jit.convertInt32ToFloat(scratchJSR.payloadGPR(), scratchFPR);
            } else {
                jit.convertInt32ToDouble(scratchJSR.payloadGPR(), scratchFPR);
            }
            done.link(&jit);
            if (isStack) {
                CCallHelpers::Address addr { calleeFrame.withOffset(wasmCallInfo.params[i].location.offsetFromSP()) };
                if (signature.argumentType(i).isF32()) {
                    jit.storeFloat(scratchFPR, addr.withOffset(PayloadOffset));
#if USE(JSVALUE32_64)
                    jit.store32(CCallHelpers::TrustedImm32(0), addr.withOffset(TagOffset));
#endif
                } else
                    jit.storeDouble(scratchFPR, addr);
            }
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    // At this point, we're committed to doing a fast call.

    if (Wasm::Context::useFastTLS()) 
        jit.loadWasmContextInstance(scratchJSR.payloadGPR());
    else
        jit.loadPtr(vm.wasmContext.pointerToInstance(), scratchJSR.payloadGPR());
    ptrdiff_t previousInstanceOffset = this->previousInstanceOffset();
    jit.storePtr(scratchJSR.payloadGPR(), CCallHelpers::Address(GPRInfo::callFrameRegister, previousInstanceOffset));

    jit.move(CCallHelpers::TrustedImmPtr(&instance()->instance()), scratchJSR.payloadGPR());
    if (Wasm::Context::useFastTLS()) 
        jit.storeWasmContextInstance(scratchJSR.payloadGPR());
    else {
        jit.move(scratchJSR.payloadGPR(), pinnedRegs.wasmContextInstancePointer);
        jit.storePtr(scratchJSR.payloadGPR(), vm.wasmContext.pointerToInstance());
    }
    if (stackLimitGPRIsClobbered)
        jit.loadPtr(vm.addressOfSoftStackLimit(), stackLimitGPR);
    jit.storePtr(stackLimitGPR, CCallHelpers::Address(scratchJSR.payloadGPR(), Wasm::Instance::offsetOfCachedStackLimit()));

#if !CPU(ARM) // ARM has no pinned registers for Wasm Memory, so no need to set them up
    if (!!instance()->instance().module().moduleInformation().memory) {
        GPRReg baseMemory = pinnedRegs.baseMemoryPointer;
        GPRReg scratchOrBoundsCheckingSize = InvalidGPRReg;
        auto mode = instance()->memoryMode();

        if (isARM64E()) {
            if (mode == MemoryMode::BoundsChecking)
                scratchOrBoundsCheckingSize = pinnedRegs.boundsCheckingSizeRegister;
            else
                scratchOrBoundsCheckingSize = stackLimitGPR;
            jit.loadPtr(CCallHelpers::Address(scratchJSR.payloadGPR(), Wasm::Instance::offsetOfCachedBoundsCheckingSize()), scratchOrBoundsCheckingSize);
        } else {
            if (mode == MemoryMode::BoundsChecking)
                jit.loadPtr(CCallHelpers::Address(scratchJSR.payloadGPR(), Wasm::Instance::offsetOfCachedBoundsCheckingSize()), pinnedRegs.boundsCheckingSizeRegister);
        }

        jit.loadPtr(CCallHelpers::Address(scratchJSR.payloadGPR(), Wasm::Instance::offsetOfCachedMemory()), baseMemory);
        jit.cageConditionallyAndUntag(Gigacage::Primitive, baseMemory, scratchOrBoundsCheckingSize, scratchJSR.payloadGPR());
    }
#endif

    // We use this callee to indicate how to unwind past these types of frames:
    // 1. We need to know where to get callee saves.
    // 2. We need to know to restore the previous wasm context.
    if (!m_jsToWasmICCallee)
        m_jsToWasmICCallee.set(vm, this, JSToWasmICCallee::create(vm, globalObject(), this));
    jit.storePtr(CCallHelpers::TrustedImmPtr(m_jsToWasmICCallee.get()), CCallHelpers::addressFor(CallFrameSlot::callee));

    // FIXME: Currently we just do an indirect jump. But we should teach the Module
    // how to repatch us:
    // https://bugs.webkit.org/show_bug.cgi?id=196570
    jit.loadPtr(entrypointLoadLocation(), scratchJSR.payloadGPR());
    jit.call(scratchJSR.payloadGPR(), WasmEntryPtrTag);

    marshallJSResult(jit, typeDefinition, wasmCallInfo, savedResultRegisters);

    ASSERT(!RegisterSetBuilder::runtimeTagRegisters().contains(GPRInfo::nonPreservedNonReturnGPR, IgnoreVectors));
    jit.loadPtr(CCallHelpers::Address(GPRInfo::callFrameRegister, previousInstanceOffset), GPRInfo::nonPreservedNonReturnGPR);
    if (Wasm::Context::useFastTLS())
        jit.storeWasmContextInstance(GPRInfo::nonPreservedNonReturnGPR);
    else
        jit.storePtr(GPRInfo::nonPreservedNonReturnGPR, vm.wasmContext.pointerToInstance());

    auto emitRestoreCalleeSaves = [&] {
        for (const RegisterAtOffset& regAtOffset : registersToSpill) {
            GPRReg reg = regAtOffset.reg().gpr();
            ASSERT(reg != GPRInfo::returnValueGPR);
            ptrdiff_t offset = regAtOffset.offset();
            jit.loadPtr(CCallHelpers::Address(GPRInfo::callFrameRegister, offset), reg);
        }
    };

    emitRestoreCalleeSaves();

    jit.emitFunctionEpilogue();
    jit.ret();

    slowPath.link(&jit);
    emitRestoreCalleeSaves();
    jit.move(CCallHelpers::TrustedImmPtr(this), GPRInfo::regT0);
    jit.emitFunctionEpilogue();
#if CPU(ARM64E)
    jit.untagReturnAddress(scratchJSR.payloadGPR());
#endif
    auto jumpToHostCallThunk = jit.jump();

    LinkBuffer linkBuffer(jit, nullptr, LinkBuffer::Profile::Wasm, JITCompilationCanFail);
    if (UNLIKELY(linkBuffer.didFailToAllocate()))
        return nullptr;

    linkBuffer.link(jumpToHostCallThunk, CodeLocationLabel<JSEntryPtrTag>(executable()->entrypointFor(CodeForCall, MustCheckArity)));
    m_jsCallEntrypoint = FINALIZE_WASM_CODE(linkBuffer, WasmEntryPtrTag, "JS->Wasm IC");
    return m_jsCallEntrypoint.code();
}

WebAssemblyFunction* WebAssemblyFunction::create(VM& vm, JSGlobalObject* globalObject, Structure* structure, unsigned length, const String& name, JSWebAssemblyInstance* instance, Wasm::Callee& jsEntrypoint, Wasm::WasmToWasmImportableFunction::LoadLocation wasmToWasmEntrypointLoadLocation, Wasm::TypeIndex typeIndex)
{
    NativeExecutable* executable = vm.getHostFunction(callWebAssemblyFunction, ImplementationVisibility::Public, WasmFunctionIntrinsic, callHostFunctionAsConstructor, nullptr, name);
    WebAssemblyFunction* function = new (NotNull, allocateCell<WebAssemblyFunction>(vm)) WebAssemblyFunction(vm, executable, globalObject, structure, jsEntrypoint, wasmToWasmEntrypointLoadLocation, typeIndex);
    function->finishCreation(vm, executable, length, name, instance);
    return function;
}

Structure* WebAssemblyFunction::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    ASSERT(globalObject);
    return Structure::create(vm, globalObject, prototype, TypeInfo(JSFunctionType, StructureFlags), info());
}

WebAssemblyFunction::WebAssemblyFunction(VM& vm, NativeExecutable* executable, JSGlobalObject* globalObject, Structure* structure, Wasm::Callee& jsEntrypoint, Wasm::WasmToWasmImportableFunction::LoadLocation wasmToWasmEntrypointLoadLocation, Wasm::TypeIndex typeIndex)
    : Base { vm, executable, globalObject, structure, Wasm::WasmToWasmImportableFunction { typeIndex, wasmToWasmEntrypointLoadLocation } }
    , m_jsEntrypoint { jsEntrypoint.entrypoint() }
{ }

template<typename Visitor>
void WebAssemblyFunction::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    WebAssemblyFunction* thisObject = jsCast<WebAssemblyFunction*>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());

    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_jsToWasmICCallee);
}

DEFINE_VISIT_CHILDREN(WebAssemblyFunction);

void WebAssemblyFunction::destroy(JSCell* cell)
{
    static_cast<WebAssemblyFunction*>(cell)->WebAssemblyFunction::~WebAssemblyFunction();
}

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
