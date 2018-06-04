///
/// Copyright (C) 2014-2015, Cyberhaven
/// All rights reserved.
///
/// Licensed under the Cyberhaven Research License Agreement.
///

#ifndef S2E_PLUGINS_LuaInstructionAnnotation_H
#define S2E_PLUGINS_LuaInstructionAnnotation_H

#include <s2e/CorePlugin.h>
#include <s2e/Plugin.h>

namespace s2e {

class S2EExecutionState;

namespace plugins {

class ModuleMap;
class ProcessExecutionDetector;
class KeyValueStore;
class FunctionMonitor;
class FunctionMonitorState;

class LuaInstructionAnnotation : public Plugin {
    S2E_PLUGIN

public:
    LuaInstructionAnnotation(S2E *s2e) : Plugin(s2e) {
    }

    struct Annotation {

        enum CallingConvention { STDCALL, CDECL, MAX_CONV };
        const std::string annotationName;
        const std::string returnAnnotationName;
        const uint64_t pc;
        const uint64_t paramCount;
        const CallingConvention convention;
        const bool fork;


        Annotation(std::string name, std::string ret_name, uint64_t pc_, uint64_t paramCount_, CallingConvention cc, bool fork_) : annotationName(name), returnAnnotationName(ret_name), pc(pc_), paramCount(paramCount_), convention(cc), fork(fork_) {
        }

        Annotation(std::string name, uint64_t pc_) : annotationName(name), returnAnnotationName(""), pc(pc_), paramCount(0), convention(CDECL),
                                                     fork(false) {
        }

        Annotation(uint64_t pc_) : Annotation("", pc_) {
        }


        bool operator==(const Annotation &a1) const {
            return pc == a1.pc && annotationName == a1.annotationName;
        }

        bool operator<(const Annotation &a1) const {
            return pc < a1.pc;
        }
    };

    void initialize();

    bool registerAnnotation(const std::string &moduleId, const Annotation &annotation);

private:


    typedef std::set<Annotation> ModuleAnnotations;
    typedef std::map<std::string, ModuleAnnotations *> Annotations;
    Annotations m_annotations;

    ProcessExecutionDetector *m_detector;
    ModuleMap *m_modules;
    KeyValueStore *m_kvs;
    FunctionMonitor *m_functionMonitor;

    sigc::connection m_instructionStart;



    void onTranslateBlockStart(ExecutionSignal *signal, S2EExecutionState *state, TranslationBlock *tb, uint64_t pc);

    void onTranslateInstructionStart(ExecutionSignal *signal, S2EExecutionState *state, TranslationBlock *tb,
                                     uint64_t pc, const ModuleAnnotations *annotations, uint64_t addend);

    void onTranslateBlockComplete(S2EExecutionState *state, TranslationBlock *tb, uint64_t ending_pc);

    void onInstruction(S2EExecutionState *state, uint64_t pc, const ModuleAnnotations *annotations, uint64_t modulePc);

    void onMonitorLoad(S2EExecutionState *state);
    void forkAnnotation(S2EExecutionState *state, const Annotation &entry);
    void onFunctionRet(S2EExecutionState *state, Annotation entry);
};

} // namespace plugins
} // namespace s2e

#endif // S2E_PLUGINS_LuaInstructionAnnotation_H
