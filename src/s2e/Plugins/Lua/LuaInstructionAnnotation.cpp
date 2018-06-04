///
/// Copyright (C) 2015, Dependable Systems Laboratory, EPFL
/// Copyright (C) 2014-2015, Cyberhaven
/// All rights reserved.
///
/// Licensed under the Cyberhaven Research License Agreement.
///

#include <s2e/ConfigFile.h>
#include <s2e/S2E.h>
#include <s2e/S2EExecutor.h>
#include <s2e/Utils.h>

#include <s2e/Plugins/OSMonitors/Support/ModuleMap.h>
#include <s2e/Plugins/OSMonitors/Support/ProcessExecutionDetector.h>
#include <s2e/Plugins/Support/KeyValueStore.h>
#include <s2e/Plugins/ExecutionMonitors/FunctionMonitor.h>

#include "LuaAnnotationState.h"
#include "LuaInstructionAnnotation.h"
#include "LuaS2EExecutionState.h"

namespace s2e {
    namespace plugins {

        S2E_DEFINE_PLUGIN(LuaInstructionAnnotation,
        "Execute Lua code on an instruction", "LuaInstructionAnnotation",
        "LuaBindings", "ProcessExecutionDetector", "ModuleMap");


        /* We use this for forking*/
        class LuaInstructionAnnotationState : public PluginState {
        private:
            bool m_child;

        public:
            LuaInstructionAnnotationState() : m_child(false){};

            virtual LuaInstructionAnnotationState *clone() const {
                return new LuaInstructionAnnotationState(*this);
            }

            static PluginState *factory(Plugin *p, S2EExecutionState *s) {
                return new LuaInstructionAnnotationState();
            }

            bool isChild() const {
                return m_child;
            }

            void makeChild(bool child) {
                m_child = child;
            }
        };


// XXX: don't duplicate with LuaFunctionAnnotation, move to ConfigFile?
        static std::string readStringOrFail(S2E *s2e, const std::string &key) {
            bool ok;
            ConfigFile *cfg = s2e->getConfig();
            std::string ret = cfg->getString(key, "", &ok);

            if (!ok) {
                s2e->getWarningsStream() << "LuaFunctionAnnotation: " << key << " is missing\n";
                exit(-1);
            }

            return ret;
        }

        static std::string readStringOrDefault(S2E *s2e, const std::string &key, const std::string defaultValue) {
            bool ok;
            ConfigFile *cfg = s2e->getConfig();
            std::string ret = cfg->getString(key, "", &ok);

            if (!ok) {
                ret=defaultValue;
            }

            return ret;
        }

        static uint64_t readIntOrFail(S2E *s2e, const std::string &key) {
            bool ok;
            ConfigFile *cfg = s2e->getConfig();
            uint64_t ret = cfg->getInt(key, 0, &ok);

            if (!ok) {
                s2e->getWarningsStream() << "LuaFunctionAnnotation: " << key << " is missing\n";
                exit(-1);
            }

            return ret;
        }

        static bool readBoolOrFail(S2E *s2e, const std::string &key) {
            bool ok;
            ConfigFile *cfg = s2e->getConfig();
            bool ret = cfg->getBool(key, 0, &ok);

            if (!ok) {
                s2e->getWarningsStream() << "LuaFunctionAnnotation: " << key << " is missing\n";
                exit(-1);
            }

            return ret;
        }


// TODO: share some code with LuaFunctionAnnotation
        void LuaInstructionAnnotation::initialize() {
            m_detector = s2e()->getPlugin<ProcessExecutionDetector>();
            m_modules = s2e()->getPlugin<ModuleMap>();
            m_kvs = s2e()->getPlugin<KeyValueStore>();
            m_functionMonitor = s2e()->getPlugin<FunctionMonitor>();

            bool ok;
            ConfigFile *cfg = s2e()->getConfig();
            ConfigFile::string_list keys = cfg->getListKeys(getConfigKey() + ".annotations", &ok);
            if (!ok) {
                getWarningsStream() << "must have an .annotations section\n";
                exit(-1);
            }

            for (auto const &key : keys) {
                std::stringstream ss;
                ss << getConfigKey() << ".annotations." << key;

                std::string moduleId = readStringOrFail(s2e(), ss.str() + ".module_name");
                std::string annotationName = readStringOrFail(s2e(), ss.str() + ".name");
                std::string returnAnnotationName = readStringOrDefault(s2e(), ss.str() + ".return_name","None");
                uint64_t pc = readIntOrFail(s2e(), ss.str() + ".pc");
                uint64_t paramCount = readIntOrFail(s2e(), ss.str() + ".param_count");
                bool fork = readBoolOrFail(s2e(), ss.str() + ".fork");
                std::string cc = readStringOrFail(s2e(), ss.str() + ".convention");

                Annotation::CallingConvention convention;
                if (cc == "stdcall") {
                    convention = Annotation::STDCALL;
                } else if (cc == "cdecl") {
                    convention = Annotation::CDECL;
                } else {
                    getWarningsStream() << "unknown calling convention" << cc << "\n";
                    exit(-1);
                }


                if (!registerAnnotation(moduleId, Annotation(annotationName, returnAnnotationName, pc, paramCount,
                                                             convention, fork))) {
                    exit(-1);
                }
            }

            m_detector->onMonitorLoad.connect(sigc::mem_fun(*this, &LuaInstructionAnnotation::onMonitorLoad));
        }

        bool LuaInstructionAnnotation::registerAnnotation(const std::string &moduleId, const Annotation &annotation) {
            if (m_annotations[moduleId] == nullptr) {
                m_annotations[moduleId] = new ModuleAnnotations();
            }

            if (m_annotations[moduleId]->find(annotation) != m_annotations[moduleId]->end()) {
                //getWarningsStream() << "attempting to register existing annotation\n";
                return true;
            }

            m_annotations[moduleId]->insert(annotation);

            getDebugStream() << "loaded " << moduleId << " " << annotation.annotationName << " "
                             << hexval(annotation.pc)
                             << "\n";

            return true;
        }

        void LuaInstructionAnnotation::onMonitorLoad(S2EExecutionState *state) {
            s2e()->getCorePlugin()->onTranslateBlockStart.connect(
                    sigc::mem_fun(*this, &LuaInstructionAnnotation::onTranslateBlockStart));

            s2e()->getCorePlugin()->onTranslateBlockComplete.connect(
                    sigc::mem_fun(*this, &LuaInstructionAnnotation::onTranslateBlockComplete));
        }

// XXX: what if TB is interrupt in the middle?
        void LuaInstructionAnnotation::onTranslateBlockStart(ExecutionSignal *signal, S2EExecutionState *state,
                                                             TranslationBlock *tb, uint64_t pc) {
            // TODO: decide here whether there might be an instruction
            // that can be hooked (probably will need the use of the static CFG)
            CorePlugin *plg = s2e()->getCorePlugin();
            m_instructionStart.disconnect();

            const ModuleDescriptor *module = m_modules->getModule(state, pc);
            if (!module) {
                return;
            }

            Annotations::const_iterator it = m_annotations.find(module->Name);
            if (it == m_annotations.end()) {
                return;
            }

            m_instructionStart = plg->onTranslateInstructionStart.connect(
                    sigc::bind(sigc::mem_fun(*this, &LuaInstructionAnnotation::onTranslateInstructionStart), it->second,
                               (-module->LoadBase +
                                module->NativeBase) /* Pass an addend to convert the program counter */
                    ));
        }

        void LuaInstructionAnnotation::onTranslateInstructionStart(ExecutionSignal *signal, S2EExecutionState *state,
                                                                   TranslationBlock *tb, uint64_t pc,
                                                                   const ModuleAnnotations *annotations,
                                                                   uint64_t addend) {
            uint64_t modulePc = pc + addend;
            Annotation tofind(modulePc);

            if (annotations->find(tofind) == annotations->end()) {
                return;
            }

            signal->connect(
                    sigc::bind(sigc::mem_fun(*this, &LuaInstructionAnnotation::onInstruction), annotations, modulePc));
        }

        void LuaInstructionAnnotation::onTranslateBlockComplete(S2EExecutionState *state, TranslationBlock *tb,
                                                                uint64_t ending_pc) {
            m_instructionStart.disconnect();
        }


        void LuaInstructionAnnotation::forkAnnotation(S2EExecutionState *state, const Annotation &entry) {
            DECLARE_PLUGINSTATE_N(LuaInstructionAnnotationState, p, state);
            if (p->isChild()) {
                return;
            }

            std::stringstream ss;
            ss << "annotation_" << entry.annotationName << "_child";

            // Use the KVS to make sure that we exercise the annotated function only once
            if (m_kvs) {
                bool exists = false;
                m_kvs->put(ss.str(), "1", exists);
                if (exists) {
                    return;
                }
            }

            klee::ref<klee::Expr> cond = state->createConcolicValue<uint8_t>(ss.str(), 0);
            cond = klee::Expr::createIsZero(cond);
            S2EExecutor::StatePair sp = s2e()->getExecutor()->forkCondition(state, cond);
            S2EExecutionState *s1 = static_cast<S2EExecutionState *>(sp.first);
            S2EExecutionState *s2 = static_cast<S2EExecutionState *>(sp.second);

            DECLARE_PLUGINSTATE_N(LuaInstructionAnnotationState, p1, s1);
            p1->makeChild(false);

            DECLARE_PLUGINSTATE_N(LuaInstructionAnnotationState, p2, s2);
            p2->makeChild(true);
        }

        void LuaInstructionAnnotation::onInstruction(S2EExecutionState *state, uint64_t pc,
                                                     const ModuleAnnotations *annotations, uint64_t modulePc) {
            if (!m_detector->isTracked(state)) {
                return;
            }

            Annotation tofind(modulePc);
            ModuleAnnotations::const_iterator it = annotations->find(tofind);
            if (it == annotations->end()) {
                return;
            }

            lua_State *L = s2e()->getConfig()->getState();

            LuaS2EExecutionState luaS2EState(state);
            LuaAnnotationState luaAnnotation;
            state->jumpToSymbolicCpp(); //allows us to write symbolic values to registers
            // must jump to symbolic before forking as we are forking upon a condition

            // if config says we should fork, we do this before the
            if (it->fork) {
                DECLARE_PLUGINSTATE_N(LuaInstructionAnnotationState, p, state);
                forkAnnotation(state, *it);

                luaAnnotation.setChild(p->isChild());
                p->makeChild(false);
            }

            getDebugStream(state) << "instruction " << hexval(modulePc) << " triggered annotation "
                                  << it->annotationName
                                  << "\n";

            // register return of this function but only if its the return Function annotationsname is not None
            if(it->returnAnnotationName.compare("None")!=0) {
                getDebugStream(state)<< "Registering return annotation: " << it->returnAnnotationName << "\n";
                // has a return Annotation so register for return signal
                FunctionMonitor::ReturnSignal returnSignal;
                returnSignal.connect(sigc::bind(sigc::mem_fun(*this, &LuaInstructionAnnotation::onFunctionRet), *it));
                getDebugStream(state) << "connected return signal \n";
                m_functionMonitor->registerReturnSignal(state, returnSignal);
                getDebugStream(state) << "registered return signal \n";
            }



            lua_getglobal(L, it->annotationName.c_str());
            Lunar<LuaS2EExecutionState>::push(L, &luaS2EState);
            Lunar<LuaAnnotationState>::push(L, &luaAnnotation);

            lua_call(L, 2, 0);

            if (luaAnnotation.exitCpuLoop()) {
                throw CpuExitException();
            }

            // only works if called directly after call instruction
            if (luaAnnotation.doSkip()) {
                getDebugStream(state) << "instruction " << hexval(modulePc) << " skipped current function "
                                      << it->annotationName
                                      << "\n";
                m_functionMonitor->eraseSp(state, state->regs()->getSp());

                if (it->convention == Annotation::STDCALL) {
                    state->bypassFunction(it->paramCount);
                } else {
                    state->bypassFunction(0);
                }

                throw CpuExitException();
            }
        }

        void LuaInstructionAnnotation::onFunctionRet(S2EExecutionState *state, Annotation entry) {
            state->jumpToSymbolicCpp();
            getDebugStream() << "Invoking return annotation " << entry.returnAnnotationName << '\n';

            lua_State *L = s2e()->getConfig()->getState();
            LuaS2EExecutionState luaS2EState(state);
            LuaAnnotationState luaAnnotation;
            state->jumpToSymbolicCpp(); //allows us to write symbolic values to registers
            lua_getglobal(L, entry.returnAnnotationName.c_str());
            Lunar<LuaS2EExecutionState>::push(L, &luaS2EState);
            Lunar<LuaAnnotationState>::push(L, &luaAnnotation);

            lua_call(L, 2, 0);

            if (luaAnnotation.exitCpuLoop()) {
                throw CpuExitException();
            }
        }

    } // namespace plugins
} // namespace s2e
