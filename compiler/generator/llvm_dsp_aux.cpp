/************************************************************************
 ************************************************************************
    FAUST compiler
	Copyright (C) 2003-2004 GRAME, Centre National de Creation Musicale
    ---------------------------------------------------------------------
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 ************************************************************************
 ************************************************************************/

#include "compatibility.hh"

#if LLVM_BUILD

#include <stdio.h>
#include <list>
#include <iostream>
#include <fstream>
#include <sstream>
#include <libgen.h>


#include "llvm_dsp_aux.hh"
#include "faust/gui/UIGlue.h"
#include "libfaust.h"
#include "dsp_aux.hh"

#if defined(LLVM_33) || defined(LLVM_34)
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/Support/system_error.h>
#include <llvm/ADT/Triple.h>
#include <llvm/Target/TargetLibraryInfo.h>
#include <llvm/Support/TargetRegistry.h>
#else
#include <llvm/Module.h>
#include <llvm/LLVMContext.h>
#include <llvm/Support/IRReader.h>
#endif

/* The file llvm/Target/TargetData.h was renamed to llvm/DataLayout.h in LLVM
 * 3.2, which itself appears to have been moved to llvm/IR/DataLayout.h in LLVM
 * 3.3.
 */
#if defined(LLVM_32)
#include <llvm/DataLayout.h>
#elif !defined(LLVM_33) && !defined(LLVM_34)
#ifndef _WIN32
#include <llvm/Target/TargetData.h>
#endif
#endif

#include <llvm/ExecutionEngine/JIT.h>
#include <llvm/PassManager.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Support/PassNameParser.h>
#include <llvm/Linker.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Assembly/PrintModulePass.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Support/Threading.h>

#ifdef LLVM_29
#include <llvm/Target/TargetSelect.h>
#endif
#if defined(LLVM_30) || defined(LLVM_31) || defined(LLVM_32) || defined(LLVM_33) || defined(LLVM_34)
#include <llvm/Support/TargetSelect.h>
#endif

using namespace llvm;

int llvm_dsp_factory::gInstance = 0;
FactoryTableType llvm_dsp_factory::gFactoryTable;

static string getParam(int argc, const char* argv[], const string& param, const string& def)
{
    for (int i = 0; i < argc; i++) {
        if (string(argv[i]) == param) return argv[i+1];
    }
    return def;
}
        
void* llvm_dsp_factory::LoadOptimize(const string& function)
{
    llvm::Function* fun_ptr = fResult->fModule->getFunction(function);
    if (fun_ptr) {
        return fJIT->getPointerToFunction(fun_ptr);
    } else {
        throw -1;
    }
}

EXPORT Module* load_single_module(const string filename, LLVMContext* context)
{
    SMDiagnostic err;
    Module* module = ParseIRFile(filename, err, *context);
    
    if (module) {
        return module;
    } else {
    #if defined(LLVM_31) || defined(LLVM_32) || defined(LLVM_33) || defined(LLVM_34)
        err.print("ParseIRFile failed :", errs());
    #else
        err.Print("ParseIRFile failed :", errs());
    #endif
        return 0;
    }
}

EXPORT bool link_modules(Module* dst, Module* src, char* error_message)
{
    string err;
    
    if (Linker::LinkModules(dst, src, Linker::DestroySource, &err)) {
        delete src;
        sprintf(error_message, "cannot link module : %s", err.c_str());
        return false;
    } else {
        delete src;
        return true;
    }
}

LLVMResult* llvm_dsp_factory::CompileModule(int argc, const char* argv[], const char* input_name, const char* input, char* error_msg)
{
    int argc1 = argc + 3;
 	const char* argv1[32];
    
    argv1[0] = "faust";
	argv1[1] = "-lang";
	argv1[2] = "llvm";
    for (int i = 0; i < argc; i++) {
        argv1[i+3] = argv[i];
    }
    
    return compile_faust_llvm(argc1, argv1, input_name, input, error_msg);
}

// Bitcode
string llvm_dsp_factory::writeDSPFactoryToBitcode()
{
    string res;
    raw_string_ostream out(res);
    WriteBitcodeToFile(fResult->fModule, out);
    out.flush();
    return res;
}

void llvm_dsp_factory::writeDSPFactoryToBitcodeFile(const string& bit_code_path)
{
    string err;
    
#if defined(LLVM_34)
    raw_fd_ostream out(bit_code_path.c_str(), err, sys::fs::F_Binary);
#else
    raw_fd_ostream out(bit_code_path.c_str(), err, raw_fd_ostream::F_Binary);
#endif
    
    WriteBitcodeToFile(fResult->fModule, out);
}

// IR
string llvm_dsp_factory::writeDSPFactoryToIR()
{
    string res;
    raw_string_ostream out(res);
    PassManager PM;
    PM.add(createPrintModulePass(&out));
    PM.run(*fResult->fModule);
    out.flush();
    return res;
}

void llvm_dsp_factory::writeDSPFactoryToIRFile(const string& ir_code_path)
{
    string err;
#if defined(LLVM_34)
    raw_fd_ostream out(ir_code_path.c_str(), err, sys::fs::F_Binary);
#else
    raw_fd_ostream out(ir_code_path.c_str(), err, raw_fd_ostream::F_Binary);
#endif
    PassManager PM;
    PM.add(createPrintModulePass(&out));
    PM.run(*fResult->fModule);
    out.flush();
}

llvm_dsp_factory::llvm_dsp_factory(const string& sha_key, Module* module, LLVMContext* context, const string& target, int opt_level)
{
    fSHAKey = sha_key;
    fOptLevel = opt_level;
    fTarget = target;
    fClassName = "mydsp";
    fExtName = "ModuleDSP";
    
    Init();
    fResult = static_cast<LLVMResult*>(calloc(1, sizeof(LLVMResult)));
    fResult->fModule = module;
    fResult->fContext = context;
}

llvm_dsp_factory::llvm_dsp_factory(const string& sha_key, int argc, const char* argv[], 
                                    const string& name_app,
                                    const string& dsp_content, 
                                    const string& target, 
                                    string& error_msg, int opt_level)
{
    if (llvm_dsp_factory::gInstance++ == 0) {
        if (!llvm_start_multithreaded()) {
            printf("llvm_start_multithreaded error...\n");
        }
    }

    // Keep given name
    fExtName = name_app;
    
    fSHAKey = sha_key;
    fOptLevel = opt_level;
    fTarget = target;
    
    Init();
    char error_msg_aux[512];
    fClassName = getParam(argc, argv, "-cn", "mydsp");
    fResult = CompileModule(argc, argv, name_app.c_str(), dsp_content.c_str(), error_msg_aux);
    error_msg = error_msg_aux;
}

void llvm_dsp_factory::Init()
{
    fJIT = 0;
    fNew = 0;
    fDelete = 0;
    fGetNumInputs = 0;
    fGetNumOutputs = 0;
    fBuildUserInterface = 0;
    fInit = 0;
    fCompute = 0;
}

llvm_dsp_aux* llvm_dsp_factory::createDSPInstance()
{
    assert(fResult->fModule);
    assert(fJIT);
    return new llvm_dsp_aux(this, fNew());
}

#if defined(LLVM_33) || defined(LLVM_34)

/// AddOptimizationPasses - This routine adds optimization passes
/// based on selected optimization level, OptLevel. This routine
/// duplicates llvm-gcc behaviour.
///
/// OptLevel - Optimization Level
static void AddOptimizationPasses(PassManagerBase &MPM,FunctionPassManager &FPM,
                                    unsigned OptLevel, unsigned SizeLevel) 
{
    FPM.add(createVerifierPass());                  // Verify that input is correct

    PassManagerBuilder Builder;
    Builder.OptLevel = OptLevel;
    Builder.SizeLevel = SizeLevel;

    if (OptLevel > 1) {
        unsigned Threshold = 225;
        if (SizeLevel == 1) {           // -Os
            Threshold = 75;
        } else if (SizeLevel == 2) {    // -Oz
            Threshold = 25;
        }
        if (OptLevel > 2) {
            Threshold = 275;
        }
        Builder.Inliner = createFunctionInliningPass(Threshold);
    } else {
        Builder.Inliner = createAlwaysInlinerPass();
    }
      
    Builder.DisableUnrollLoops = OptLevel == 0;
#if defined(LLVM_33)   
    Builder.DisableSimplifyLibCalls = false;
#endif
      
    if (OptLevel > 3) {
        Builder.LoopVectorize = true;
        Builder.SLPVectorize = true;
    }
    if (OptLevel > 4) {
        Builder.BBVectorize = true;
    }
     
    Builder.populateFunctionPassManager(FPM);
    Builder.populateModulePassManager(MPM);
}

bool llvm_dsp_factory::initJIT(string& error_msg)
{
    // First check is Faust compilation succeeded... (valid LLVM module)
    if (!fResult || !fResult->fModule) {
        return false;
    }
     
    InitializeAllTargets();
    InitializeAllTargetMCs();
  
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    // Initialize passes
    PassRegistry &Registry = *PassRegistry::getPassRegistry();
    
    initializeCodeGen(Registry);
    initializeCore(Registry);
    initializeScalarOpts(Registry);
    initializeObjCARCOpts(Registry);
    initializeVectorization(Registry);
    initializeIPO(Registry);
    initializeAnalysis(Registry);
    initializeIPA(Registry);
    initializeTransformUtils(Registry);
    initializeInstCombine(Registry);
    initializeInstrumentation(Registry);
    initializeTarget(Registry);
    
    string err;
      
    if (fTarget != "") {
        fResult->fModule->setTargetTriple(fTarget);
    } else {
        fResult->fModule->setTargetTriple(llvm::sys::getDefaultTargetTriple());
    }

    EngineBuilder builder(fResult->fModule);
    builder.setOptLevel(CodeGenOpt::Aggressive);
    builder.setEngineKind(EngineKind::JIT);
    // MCJIT does not work correctly (incorrect float numbers ?) when used with dynamic libLLVM
    builder.setUseMCJIT(true);
    //builder.setUseMCJIT(false);
    builder.setCodeModel(CodeModel::JITDefault);
    builder.setMCPU(llvm::sys::getHostCPUName());
    
    TargetOptions targetOptions;
    targetOptions.NoFramePointerElim = true;
    targetOptions.LessPreciseFPMADOption = true;
    targetOptions.UnsafeFPMath = true;
    targetOptions.NoInfsFPMath = true;
    targetOptions.NoNaNsFPMath = true;
    targetOptions.GuaranteedTailCallOpt = true;
     
    string debug_var = (getenv("FAUST_DEBUG")) ? string(getenv("FAUST_DEBUG")) : "";
    
    if ((debug_var != "") && (debug_var.find("FAUST_LLVM3") != string::npos)) {
       targetOptions.PrintMachineCode = true;
    }
    
    builder.setTargetOptions(targetOptions);
    TargetMachine* tm = builder.selectTarget();
    
    PassManager pm;
    FunctionPassManager fpm(fResult->fModule);
  
    // Add an appropriate TargetLibraryInfo pass for the module's triple.
    TargetLibraryInfo* tli = new TargetLibraryInfo(Triple(fResult->fModule->getTargetTriple()));
    pm.add(tli);
    
    const string &ModuleDataLayout = fResult->fModule->getDataLayout();
    DataLayout* td = new DataLayout(ModuleDataLayout);
    pm.add(td);
  
    // Add internal analysis passes from the target machine (mandatory for vectorization to work)
    tm->addAnalysisPasses(pm);
    
    if (fOptLevel > 0) {
        AddOptimizationPasses(pm, fpm, fOptLevel, 0);
    }
    
    if ((debug_var != "") && (debug_var.find("FAUST_LLVM1") != string::npos)) {
        fResult->fModule->dump();
    }
   
    fpm.doInitialization();
    for (Module::iterator F = fResult->fModule->begin(), E = fResult->fModule->end(); F != E; ++F) {
        fpm.run(*F);
    }
    fpm.doFinalization();
    
    pm.add(createVerifierPass());
    
    if ((debug_var != "") && (debug_var.find("FAUST_LLVM4") != string::npos)) {
        tm->addPassesToEmitFile(pm, fouts(), TargetMachine::CGFT_AssemblyFile, true);
    }
    
    // Now that we have all of the passes ready, run them.
    pm.run(*fResult->fModule);
    
    if ((debug_var != "") && (debug_var.find("FAUST_LLVM2") != string::npos)) {
        fResult->fModule->dump();
    }
    
    fJIT = builder.create(tm);
    if (!fJIT) {
        return false;
    }
    
    // Run static constructors.
    fJIT->runStaticConstructorsDestructors(false);
    fJIT->DisableLazyCompilation(true);
    
    try {
        fNew = (newDspFun)LoadOptimize("new_" + fClassName);
        fDelete = (deleteDspFun)LoadOptimize("delete_" + fClassName);
        fGetNumInputs = (getNumInputsFun)LoadOptimize("getNumInputs_" + fClassName);
        fGetNumOutputs = (getNumOutputsFun)LoadOptimize("getNumOutputs_" + fClassName);
        fBuildUserInterface = (buildUserInterfaceFun)LoadOptimize("buildUserInterface_" + fClassName);
        fInit = (initFun)LoadOptimize("init_" + fClassName);
        fCompute = (computeFun)LoadOptimize("compute_" + fClassName);
        fMetadata = (metadataFun)LoadOptimize("metadata_" + fClassName);
        return true;
    } catch (...) { // Module does not contain the Faust entry points...
        return false;
    }
}

#else

bool llvm_dsp_factory::initJIT(string& error_msg)
{
    // First check is Faust compilation succeeded... (valid LLVM module)
    if (!fResult || !fResult->fModule) {
        return false;
    }
    
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();
    
    if (fTarget != "") {
         fResult->fModule->setTargetTriple(fTarget);
    } else {
    #if defined(LLVM_31) || defined(LLVM_32) 
        fResult->fModule->setTargetTriple(llvm::sys::getDefaultTargetTriple());
    #else
        fResult->fModule->setTargetTriple(llvm::sys::getHostTriple());
    #endif
    }

    string err;
    EngineBuilder builder(fResult->fModule);
    builder.setOptLevel(CodeGenOpt::Aggressive);
    builder.setEngineKind(EngineKind::JIT);
    // MCJIT does not work correctly (incorrect float numbers ?) when used with dynamic libLLVM
    //builder.setUseMCJIT(true);
    builder.setUseMCJIT(false);
    builder.setMCPU(llvm::sys::getHostCPUName());
       
#ifndef LLVM_30
    TargetMachine* tm = builder.selectTarget();
#endif
    //tm->Options.PrintMachineCode = 1;
    /*
    SmallVector<string, 4> attrs;
    attrs.push_back("sse");
    attrs.push_back("sse2");
    attrs.push_back("sse3");
    attrs.push_back("enable-unsafe-fp-math");
    builder.setMAttrs(attrs);
    */
#ifdef LLVM_30
    fJIT = builder.create();
#else
    fJIT = builder.create(tm);
#endif
    
    if (!fJIT) {
        return false;
    }
    
    // Run static constructors.
    fJIT->runStaticConstructorsDestructors(false);
    
    fJIT->DisableLazyCompilation(true);
#if defined(LLVM_32) 
    fResult->fModule->setDataLayout(fJIT->getDataLayout()->getStringRepresentation());
#else
    fResult->fModule->setDataLayout(fJIT->getTargetData()->getStringRepresentation());
#endif
    
    // Set up the optimizer pipeline. Start with registering info about how the
    // target lays out data structures.
    PassManager pm;
    FunctionPassManager fpm(fResult->fModule);
#if defined(LLVM_32)    
    // TODO
#else
    pm.add(new TargetData(*fJIT->getTargetData()));
    fpm.add(new TargetData(*fJIT->getTargetData()));
#endif

    // Taken from LLVM Opt.cpp
    PassManagerBuilder Builder;
    Builder.OptLevel = fOptLevel;

    if (fOptLevel > 1) {
        unsigned threshold = 225;
        if (fOptLevel > 2) {
            threshold = 275;
        }
        Builder.Inliner = createFunctionInliningPass(threshold);
    } else {
        Builder.Inliner = createAlwaysInlinerPass();
    }
    
    // We use '4' to activate the auto-vectorizer
    if (fOptLevel > 3) {
    
    #if defined(LLVM_32) 
        printf("Vectorize\n");
        Builder.LoopVectorize = true;
        //Builder.Vectorize = true;
    #elif defined(LLVM_31)
        Builder.Vectorize = true;
    #endif
    }
      
    Builder.DisableUnrollLoops = (fOptLevel == 0);
    Builder.populateFunctionPassManager(fpm);
    Builder.populateModulePassManager(pm);
    
    string debug_var = (getenv("FAUST_DEBUG")) ? string(getenv("FAUST_DEBUG")) : "";
    
    if ((debug_var != "") && (debug_var.find("FAUST_LLVM1") != string::npos)) {
        fResult->fModule->dump();
    }
    
    // Now that we have all of the passes ready, run them.
    pm.run(*fResult->fModule);
    
    if ((debug_var != "") && (debug_var.find("FAUST_LLVM2") != string::npos)) {
        fResult->fModule->dump();
    }
     
    try {
        fNew = (newDspFun)LoadOptimize("new_" + fClassName);
        fDelete = (deleteDspFun)LoadOptimize("delete_" + fClassName);
        fGetNumInputs = (getNumInputsFun)LoadOptimize("getNumInputs_" + fClassName);
        fGetNumOutputs = (getNumOutputsFun)LoadOptimize("getNumOutputs_" + fClassName);
        fBuildUserInterface = (buildUserInterfaceFun)LoadOptimize("buildUserInterface_" + fClassName);
        fInit = (initFun)LoadOptimize("init_" + fClassName);
        fCompute = (computeFun)LoadOptimize("compute_" + fClassName);
        fMetadata = (metadataFun)LoadOptimize("metadata_" + fClassName);
        return true;
    } catch (...) { // Module does not contain the Faust entry points...
        return false;
    }
}

#endif

llvm_dsp_factory::~llvm_dsp_factory()
{
    if (fJIT) {
        fJIT->runStaticConstructorsDestructors(true);
        // fResult->fModule is kept and deleted by fJIT
        delete fJIT;
    }
    
    if (fResult) {
        delete fResult->fContext;
        free(fResult);
    }
    
    if (--llvm_dsp_factory::gInstance == 0) {
        llvm_stop_multithreaded();
    }
}

void llvm_dsp_factory::metadataDSPFactory(Meta* meta)
{
    MetaGlue glue;
    buildMetaGlue(&glue, meta);
    fMetadata(&glue);
}

void llvm_dsp_factory::metadataDSPFactory(MetaGlue* glue)
{
    fMetadata(glue);
}

string llvm_dsp_factory::getName()
{
    struct MyMeta : public Meta
    {
        string name;
        
        virtual void declare(const char* key, const char* value){
            if (strcmp(key, "name") == 0) {
                name = value;
            }
        }
    };
    
    MyMeta metadata;
    metadataDSPFactory (&metadata);
    return (fExtName != "") ? fExtName : metadata.name;
 }
  
// Instance 

llvm_dsp_aux::llvm_dsp_aux(llvm_dsp_factory* factory, llvm_dsp_imp* dsp)
    :fDSPFactory(factory), fDSP(dsp)
{
    assert(fDSPFactory);
    assert(fDSP);
}
        
llvm_dsp_aux::~llvm_dsp_aux()
{   
    if (fDSP) {
        fDSPFactory->fDelete(fDSP);
    }
}

void llvm_dsp_aux::metadata(Meta* m)
{
    MetaGlue glue;
    buildMetaGlue(&glue, m);
    return fDSPFactory->fMetadata(&glue);
}

void llvm_dsp_aux::metadata(MetaGlue* m)
{
    return fDSPFactory->fMetadata(m);
}

int llvm_dsp_aux::getNumInputs()
{
    return fDSPFactory->fGetNumInputs(fDSP);
}
int llvm_dsp_aux::getNumOutputs()
{
    return fDSPFactory->fGetNumOutputs(fDSP);
}

void llvm_dsp_aux::init(int samplingFreq)
{
    fDSPFactory->fInit(fDSP, samplingFreq);
}

void llvm_dsp_aux::buildUserInterface(UI* interface)
{
    UIGlue glue;
    buildUIGlue(&glue, interface);
    fDSPFactory->fBuildUserInterface(fDSP, &glue);
}

void llvm_dsp_aux::buildUserInterface(UIGlue* glue)
{
    fDSPFactory->fBuildUserInterface(fDSP, glue);
}

void llvm_dsp_aux::compute(int count, FAUSTFLOAT** input, FAUSTFLOAT** output)
{
    AVOIDDENORMALS;
    fDSPFactory->fCompute(fDSP, count, input, output);
}

static llvm_dsp_factory* CheckDSPFactory(llvm_dsp_factory* factory, string& error_msg)
{   
    if (factory->initJIT(error_msg)) {
        return factory;
    } else {
        delete factory;
        return 0;
    }
}

EXPORT string path_to_content(const string& path)
{
    ifstream file(path.c_str(), ifstream::binary);
    
    file.seekg (0, file.end);
    int size = file.tellg();
    file.seekg (0, file.beg);
    
    // And allocate buffer to that a single line can be read...
    char* buffer = new char[size + 1];
    file.read(buffer, size);
    
    // Terminate the string
    buffer[size] = 0;
    string result = buffer;
    file.close();
    delete [] buffer;
    return result;
}

static bool getFactory(const string& sha_key, FactoryTableIt& res)
{
    FactoryTableIt it;
    
    for (it = llvm_dsp_factory::gFactoryTable.begin(); it != llvm_dsp_factory::gFactoryTable.end(); it++) {
        if ((*it).first->getSHAKey() == sha_key) {
            res = it;
            return true;
        }
    }
    
    return false;
}

// Public C++ API

EXPORT llvm_dsp_factory* createDSPFactory(int argc, const char* argv[], 
                                        const string& name, 
                                        const string& input, const string& target, 
                                        string& error_msg, int opt_level)
{
    return CheckDSPFactory(new llvm_dsp_factory("", argc, argv, name, input, target, error_msg, opt_level), error_msg);
}

EXPORT llvm_dsp_factory* createDSPFactoryFromFile(const string& filename, 
                                                int argc, const char* argv[], 
                                                const string& target, 
                                                string& error_msg, int opt_level)
{
    string base = basename((char*)filename.c_str());
    int pos = base.find(".dsp");
    
    if (pos != string::npos) {
        return createDSPFactoryFromString(base.substr(0, pos), path_to_content(filename), argc, argv, target, error_msg, opt_level);
    } else {
        error_msg = "File Extension is not the one expected (.dsp expected)\n";
        return NULL;
    } 
}

EXPORT llvm_dsp_factory* createDSPFactoryFromString(const string& name_app, const string& dsp_content, 
                                                    int argc, const char* argv[], 
                                                    const string& target, 
                                                    string& error_msg, int opt_level)
{
    /*
    string expanded_dsp;
    string sha_key;
    
    if ((expanded_dsp = expandDSPFromString(name_app, dsp_content, argc, argv, sha_key, error_msg)) == "") {
        return 0; 
    } else {
        FactoryTableIt it;
        llvm_dsp_factory* factory = 0;
        if (getFactory(sha_key, it)) {
            Sllvm_dsp_factory sfactory = (*it).first;
            sfactory->addReference();
            return sfactory;
        } else if ((factory = CheckDSPFactory(new llvm_dsp_factory(sha_key, argc, argv, name_app, dsp_content, target, error_msg, opt_level), error_msg)) != 0) {
            llvm_dsp_factory::gFactoryTable[factory] = list<llvm_dsp_aux*>();
            return factory;
        } else {
            return 0;
        }
    }
    */
    
    string sha_key = generate_sha1(dsp_content);
    FactoryTableIt it;
    llvm_dsp_factory* factory = 0;
    
    if (getFactory(sha_key, it)) {
        Sllvm_dsp_factory sfactory = (*it).first;
        sfactory->addReference();
        return sfactory;
    } else if ((factory = CheckDSPFactory(new llvm_dsp_factory(sha_key, argc, argv, name_app, dsp_content, target, error_msg, opt_level), error_msg)) != 0) {
        llvm_dsp_factory::gFactoryTable[factory] = list<llvm_dsp_aux*>();
        return factory;
    } else {
        return 0;
    }
}

EXPORT llvm_dsp_factory* createDSPFactoryFromSHAKey(const string& sha_key)
{
    FactoryTableIt it;
    return (getFactory(sha_key, it)) ? (*it).first : NULL;
}

EXPORT vector<string> getAllDSPFactories()
{
    FactoryTableIt it;
    vector<string> sha_key_list;
    
    for (it = llvm_dsp_factory::gFactoryTable.begin(); it != llvm_dsp_factory::gFactoryTable.end(); it++) {
        sha_key_list.push_back((*it).first->getSHAKey());
    }
    
    return sha_key_list;
}

EXPORT void deleteDSPFactory(llvm_dsp_factory* factory) 
{   
    FactoryTableIt it;
    if ((it = llvm_dsp_factory::gFactoryTable.find(factory)) != llvm_dsp_factory::gFactoryTable.end()) {
        Sllvm_dsp_factory sfactory = (*it).first;
        if (sfactory->refs() == 2) { // Local stack pointer + the one in gFactoryTable...
            // Last use, remove from the global table, pointer will be deleted
            llvm_dsp_factory::gFactoryTable.erase(factory);
        } else {
            sfactory->removeReference();
        }
    }
}

EXPORT void deleteAllDSPFactories()
{
    FactoryTableIt it;
    for (it = llvm_dsp_factory::gFactoryTable.begin(); it != llvm_dsp_factory::gFactoryTable.end(); it++) {
        // Force deletion of factory...
        delete (*it).first;
    }
}
    

// Bitcode <==> string

static llvm_dsp_factory* readDSPFactoryFromBitcodeAux(MemoryBuffer* buffer, const string& target, int opt_level)
{
    string sha_key = generate_sha1(buffer->getBuffer().str());
    FactoryTableIt it;
    
    if (getFactory(sha_key, it)) {
        Sllvm_dsp_factory sfactory = (*it).first;
        sfactory->addReference();
        return sfactory;
    } else {
        string error_msg;
        LLVMContext* context = new LLVMContext();
        Module* module = ParseBitcodeFile(buffer, *context, &error_msg);
        llvm_dsp_factory* factory = 0;
        if (module && ((factory = CheckDSPFactory(new llvm_dsp_factory(sha_key, module, context, target, opt_level), error_msg)) != 0)) {
            llvm_dsp_factory::gFactoryTable[factory] = list<llvm_dsp_aux*>();
            return factory;
        } else {
            printf("readDSPFactoryFromBitcode failed : %s\n", error_msg.c_str());
            return 0;
        }
    }
}

EXPORT llvm_dsp_factory* readDSPFactoryFromBitcode(const string& bit_code, const string& target, int opt_level)
{
    return readDSPFactoryFromBitcodeAux(MemoryBuffer::getMemBuffer(StringRef(bit_code)), target, opt_level);
}

EXPORT string writeDSPFactoryToBitcode(llvm_dsp_factory* factory)
{
    return factory->writeDSPFactoryToBitcode();
}

// Bitcode <==> file
EXPORT llvm_dsp_factory* readDSPFactoryFromBitcodeFile(const string& bit_code_path, const string& target, int opt_level)
{
    OwningPtr<MemoryBuffer> buffer;
    if (llvm::error_code ec = MemoryBuffer::getFileOrSTDIN(bit_code_path.c_str(), buffer)) {
        printf("readDSPFactoryFromBitcodeFile failed : %s\n", ec.message().c_str());
        return 0;
    } else {
        return readDSPFactoryFromBitcodeAux(buffer.get(), target, opt_level);
    }
}

EXPORT void writeDSPFactoryToBitcodeFile(llvm_dsp_factory* factory, const string& bit_code_path)
{
    factory->writeDSPFactoryToBitcodeFile(bit_code_path);
}

// IR <==> string

static llvm_dsp_factory* readDSPFactoryFromIRAux(MemoryBuffer* buffer, const string& target, int opt_level)
{
    string sha_key = generate_sha1(buffer->getBuffer().str());
    FactoryTableIt it;
    
    if (getFactory(sha_key, it)) {
        Sllvm_dsp_factory sfactory = (*it).first;
        sfactory->addReference();
        return sfactory;
    } else {
        char* tmp_local = setlocale(LC_ALL, NULL);
        setlocale(LC_ALL, "C");
        LLVMContext* context = new LLVMContext();
        SMDiagnostic err;
        Module* module = ParseIR(buffer, err, *context); // ParseIR takes ownership of the given buffer, so don't delete it
        setlocale(LC_ALL, tmp_local);
        llvm_dsp_factory* factory = 0;
        string error_msg;
        if (module && ((factory = CheckDSPFactory(new llvm_dsp_factory(sha_key, module, context, target, opt_level), error_msg)) != 0)) {
            llvm_dsp_factory::gFactoryTable[factory] = list<llvm_dsp_aux*>();
            return factory;
        } else {
        #if defined(LLVM_31) || defined(LLVM_32) || defined(LLVM_33) || defined(LLVM_34)
            err.print("readDSPFactoryFromIRAux failed :", errs());
        #else
            err.Print("readDSPFactoryFromIRAux failed :", errs());
        #endif
            return 0;
        }
    }
}

EXPORT llvm_dsp_factory* readDSPFactoryFromIR(const string& ir_code, const string& target, int opt_level)
{
    return readDSPFactoryFromIRAux(MemoryBuffer::getMemBuffer(StringRef(ir_code)), target, opt_level);
}

EXPORT string writeDSPFactoryToIR(llvm_dsp_factory* factory)
{
    return factory->writeDSPFactoryToIR();
}

// IR <==> file
EXPORT llvm_dsp_factory* readDSPFactoryFromIRFile(const string& ir_code_path, const string& target, int opt_level)
{
    OwningPtr<MemoryBuffer> buffer;
    if (llvm::error_code ec = MemoryBuffer::getFileOrSTDIN(ir_code_path.c_str(), buffer)) {
        printf("readDSPFactoryFromIRFile failed : %s\n", ec.message().c_str());
        return 0;
    } else {
        return readDSPFactoryFromIRAux(buffer.get(), target, opt_level);
    }
}

EXPORT void writeDSPFactoryToIRFile(llvm_dsp_factory* factory, const string& ir_code_path)
{
    factory->writeDSPFactoryToIRFile(ir_code_path);
}

EXPORT void metadataDSPFactory(llvm_dsp_factory* factory, Meta* m)
{
    factory->metadataDSPFactory(m);
}

// Instance

EXPORT llvm_dsp* createDSPInstance(llvm_dsp_factory* factory)
{  
    FactoryTableIt it;
    if ((it = llvm_dsp_factory::gFactoryTable.find(factory)) != llvm_dsp_factory::gFactoryTable.end()) {
        llvm_dsp_aux* instance = factory->createDSPInstance();
        (*it).second.push_back(instance);
        return reinterpret_cast<llvm_dsp*>(instance);
    } else {
        return 0;
    }
}

EXPORT void deleteDSPInstance(llvm_dsp* dsp) 
{
    FactoryTableIt it;
    llvm_dsp_aux* dsp_aux = reinterpret_cast<llvm_dsp_aux*>(dsp);
    llvm_dsp_factory* factory = dsp_aux->getFactory();
    
    it = llvm_dsp_factory::gFactoryTable.find(factory);
    assert(it != llvm_dsp_factory::gFactoryTable.end());
    (*it).second.remove(dsp_aux);
     
    delete dsp_aux; 
}

EXPORT void llvm_dsp::metadata(Meta* m)
{
    reinterpret_cast<llvm_dsp_aux*>(this)->metadata(m);
}

EXPORT int llvm_dsp::getNumInputs()
{
    return reinterpret_cast<llvm_dsp_aux*>(this)->getNumInputs();
}

int EXPORT llvm_dsp::getNumOutputs()
{
    return reinterpret_cast<llvm_dsp_aux*>(this)->getNumOutputs();
}

EXPORT void llvm_dsp::init(int samplingFreq)
{
    reinterpret_cast<llvm_dsp_aux*>(this)->init(samplingFreq);
}

EXPORT void llvm_dsp::buildUserInterface(UI* interface)
{
    reinterpret_cast<llvm_dsp_aux*>(this)->buildUserInterface(interface);
}

EXPORT void llvm_dsp::compute(int count, FAUSTFLOAT** input, FAUSTFLOAT** output)
{
    reinterpret_cast<llvm_dsp_aux*>(this)->compute(count, input, output);
}

// Public C interface

EXPORT llvm_dsp_factory* createCDSPFactoryFromSHAKey(const char* sha_key)
{
    return createDSPFactoryFromSHAKey(sha_key);
}

EXPORT llvm_dsp_factory* createCDSPFactory(int argc, const char* argv[], 
                                           const char* name, const char* input, 
                                           const char* target, char* error_msg, int opt_level)
{
    string error_msg_aux;
    llvm_dsp_factory* factory = CheckDSPFactory(new llvm_dsp_factory("", argc, argv, name, input, target, error_msg_aux, opt_level), error_msg_aux);
    strncpy(error_msg, error_msg_aux.c_str(), 256);
    return factory;
}

EXPORT llvm_dsp_factory* createCDSPFactoryFromFile(const char* filename, 
                                                   int argc, const char* argv[], 
                                                   const char* target, 
                                                   char* error_msg, int opt_level)
{
    string error_msg_aux;
    llvm_dsp_factory* factory = createDSPFactoryFromFile(filename, argc, argv, target, error_msg_aux, opt_level);
    strncpy(error_msg, error_msg_aux.c_str(), 256);
    return factory;                                
}

EXPORT llvm_dsp_factory* createCDSPFactoryFromString(const char* name_app, const char* dsp_content, 
                                                     int argc, const char* argv[], 
                                                     const char* target, 
                                                     char* error_msg, int opt_level)
{
    string error_msg_aux;
    llvm_dsp_factory* factory = createDSPFactoryFromString(name_app, dsp_content, argc, argv, target, error_msg_aux, opt_level);
    strncpy(error_msg, error_msg_aux.c_str(), 256);
    return factory;
}

EXPORT const char** getAllCDSPFactories()
{
    vector<string> sha_key_list1 = getAllDSPFactories();
    const char** sha_key_list2 = (const char**)malloc(sizeof(char*) * (sha_key_list1.size() + 1));
    
    int i;
    for (i = 0; i < sha_key_list1.size(); i++) {
        string sha_key1 = sha_key_list1[i];
        char* sha_key2 = (char*)malloc(sizeof(char) * (sha_key1.length() + 1));
        strcpy(sha_key2, sha_key1.c_str());
        sha_key_list2[i] = sha_key2;
    }
    
    // Last element is NULL
    sha_key_list2[i] = NULL;
    return sha_key_list2;
}

EXPORT void deleteCDSPFactory(llvm_dsp_factory* factory)
{
    deleteDSPFactory(factory);
}

EXPORT void deleteAllCDSPFactories()
{
    deleteAllDSPFactories();
}

EXPORT llvm_dsp_factory* readCDSPFactoryFromBitcode(const char* bit_code, const char* target, int opt_level)
{
    return readDSPFactoryFromBitcode(bit_code, target, opt_level);
}

EXPORT const char* writeCDSPFactoryToBitcode(llvm_dsp_factory* factory)
{
    string str = writeDSPFactoryToBitcode(factory);
    char* cstr = (char*)malloc(str.length() + 1);
    strcpy(cstr, str.c_str());
    return cstr;
}

EXPORT llvm_dsp_factory* readCDSPFactoryFromBitcodeFile(const char* bit_code_path, const char* target, int opt_level)
{
    return readDSPFactoryFromBitcodeFile(bit_code_path, target, opt_level);
}

EXPORT void writeCDSPFactoryToBitcodeFile(llvm_dsp_factory* factory, const char* bit_code_path)
{
    writeDSPFactoryToBitcodeFile(factory, bit_code_path);
}

EXPORT llvm_dsp_factory* readCDSPFactoryFromIR(const char* ir_code, const char* target, int opt_level)
{
    return readDSPFactoryFromIR(ir_code, target, opt_level);
}

EXPORT const char* writeCDSPFactoryToIR(llvm_dsp_factory* factory)
{
    string str = writeDSPFactoryToIR(factory);
    char* cstr = (char*)malloc(str.length() + 1);
    strcpy(cstr, str.c_str());
    return cstr;
}

EXPORT llvm_dsp_factory* readCDSPFactoryFromIRFile(const char* ir_code_path, const char* target, int opt_level)
{
    return readDSPFactoryFromIRFile(ir_code_path, target, opt_level);
}

EXPORT void writeCDSPFactoryToIRFile(llvm_dsp_factory* factory, const char* ir_code_path)
{
    writeDSPFactoryToIRFile(factory, ir_code_path);
}

EXPORT void metadataCDSPFactory(llvm_dsp_factory* factory, MetaGlue* glue)
{
    factory->metadataDSPFactory(glue);
}

EXPORT void metadataCDSPInstance(llvm_dsp* dsp, MetaGlue* glue)
{
    reinterpret_cast<llvm_dsp_aux*>(dsp)->metadata(glue);
}

EXPORT int getNumInputsCDSPInstance(llvm_dsp* dsp)
{
    return reinterpret_cast<llvm_dsp_aux*>(dsp)->getNumInputs();
}

EXPORT int getNumOutputsCDSPInstance(llvm_dsp* dsp)
{
    return reinterpret_cast<llvm_dsp_aux*>(dsp)->getNumOutputs();
}

EXPORT void initCDSPInstance(llvm_dsp* dsp, int samplingFreq)
{
    reinterpret_cast<llvm_dsp_aux*>(dsp)->init(samplingFreq);
}

EXPORT void buildUserInterfaceCDSPInstance(llvm_dsp* dsp, UIGlue* glue)
{
    reinterpret_cast<llvm_dsp_aux*>(dsp)->buildUserInterface(glue);
}

EXPORT void computeCDSPInstance(llvm_dsp* dsp, int count, FAUSTFLOAT** input, FAUSTFLOAT** output)
{
    reinterpret_cast<llvm_dsp_aux*>(dsp)->compute(count, input, output);
}

EXPORT llvm_dsp* createCDSPInstance(llvm_dsp_factory* factory)
{
    return reinterpret_cast<llvm_dsp*>(factory->createDSPInstance());
}

EXPORT void deleteCDSPInstance(llvm_dsp* dsp)
{
    delete reinterpret_cast<llvm_dsp_aux*>(dsp); 
}

#endif // LLVM_BUILD
