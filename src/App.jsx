import React, { useState, useEffect, useRef, useCallback } from 'react';
import Header from './components/Header';
import Editor from './components/Editor';
import TokensPanel from './components/TokensPanel';
import ASTPanel from './components/ASTPanel';
import SymbolTablePanel from './components/SymbolTablePanel';
import TACPanel from './components/TACPanel';
import BytecodePanel from './components/BytecodePanel';
import VMVisualizer from './components/VMVisualizer';
import ConsoleOutput from './components/ConsoleOutput';
import { PRESET_PROGRAMS } from './engine/presets';
import { compileCSource } from './engine/compilerEngine';
import { Code, Layers, Cpu, Database, Zap, Binary, PlayCircle } from 'lucide-react';

export default function App() {
  const [selectedPreset, setSelectedPreset] = useState('hello_world');
  const [code, setCode] = useState(PRESET_PROGRAMS.hello_world.code);
  const [backendMode, setBackendMode] = useState('browser');
  const [activeTab, setActiveTab] = useState('execution');
  const [isCompiling, setIsCompiling] = useState(false);
  const [compileResult, setCompileResult] = useState(null);
  // which engine actually produced the current result (may differ from the
  // requested mode when the native backend is unreachable and we fall back)
  const [activeEngine, setActiveEngine] = useState(null);
  const [fallbackNotice, setFallbackNotice] = useState('');
  const [activeLine, setActiveLine] = useState(null);
  const [errorLine, setErrorLine] = useState(null);
  const [consoleHeight, setConsoleHeight] = useState(200);
  const [terminalInputs, setTerminalInputs] = useState([]);
  const abortRef = useRef(null);

  const handleCompile = useCallback(async (sourceToCompile = code, inputs = terminalInputs, mode = backendMode) => {
    // cancel any in-flight native request
    if (abortRef.current) { try { abortRef.current.abort(); } catch { /* noop */ } }

    setIsCompiling(true);
    setFallbackNotice('');
    let result = null;
    let engineUsed = null;

    if (mode === 'native') {
      const controller = new AbortController();
      abortRef.current = controller;
      try {
        const response = await fetch('/api/compile', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ source: sourceToCompile, inputs }),
          signal: controller.signal
        });
        if (!response.ok) throw new Error(`Native backend responded ${response.status}`);
        const data = await response.json();
        result = data;
        engineUsed = 'native';
      } catch (err) {
        if (err.name === 'AbortError') { setIsCompiling(false); return; }
        // explicit, visible fallback to the browser engine
        setFallbackNotice('Native backend unreachable — using the in-browser engine instead.');
        result = compileCSource(sourceToCompile, inputs);
        engineUsed = 'browser';
      }
    } else {
      result = compileCSource(sourceToCompile, inputs);
      engineUsed = 'browser';
    }

    setCompileResult(result);
    setActiveEngine(engineUsed);
    setActiveLine(null);
    const firstError = (result?.diagnostics || []).find((d) => d.level === 'error');
    setErrorLine(firstError && firstError.line > 0 ? firstError.line : null);
    setIsCompiling(false);
  }, [code, terminalInputs, backendMode]);

  useEffect(() => {
    handleCompile(PRESET_PROGRAMS.hello_world.code, [], 'browser');
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const handleSelectPreset = (key) => {
    setSelectedPreset(key);
    if (PRESET_PROGRAMS[key]) {
      setCode(PRESET_PROGRAMS[key].code);
      setTerminalInputs([]);
      handleCompile(PRESET_PROGRAMS[key].code, [], backendMode);
    }
  };

  const handleTerminalInput = (inputVal) => {
    const updatedInputs = [...terminalInputs, inputVal];
    setTerminalInputs(updatedInputs);
    handleCompile(code, updatedInputs, backendMode);
  };

  const handleFreshCompile = () => {
    setTerminalInputs([]);
    handleCompile(code, [], backendMode);
  };

  const tabs = [
    { id: 'execution', label: '07. VM Execution Visualizer', icon: PlayCircle, badge: 'Phase 7' },
    { id: 'tokens', label: '01. Tokens', icon: Code, badge: compileResult?.tokens?.length },
    { id: 'ast', label: '02. AST Tree', icon: Layers, badge: 'Phase 2' },
    { id: 'symbols', label: '03. Symbol Table', icon: Database, badge: compileResult?.symbolTable?.length },
    { id: 'tac', label: '04. TAC & Optimization', icon: Zap, badge: compileResult ? `${compileResult.metrics?.reduction_percentage ?? 0}%` : '' },
    { id: 'bytecode', label: '05. Bytecode', icon: Binary, badge: compileResult?.bytecode?.length }
  ];

  return (
    <div className="flex flex-col h-screen bg-[#181818] text-[#cccccc] overflow-hidden">
      <Header
        selectedPreset={selectedPreset}
        onSelectPreset={handleSelectPreset}
        onCompile={handleFreshCompile}
        isCompiling={isCompiling}
        backendMode={backendMode}
        setBackendMode={setBackendMode}
        activeEngine={activeEngine}
        compileTimeMs={compileResult?.compile_time_ms}
      />

      {fallbackNotice && (
        <div role="status" className="bg-amber-950/60 border-b border-amber-700/50 text-amber-300 text-[11px] px-4 py-1.5">
          {fallbackNotice}
        </div>
      )}

      {/* Main Studio Workspace */}
      <div className="flex-1 flex overflow-hidden">
        {/* Left Side: editor */}
        <div className="w-1/2 h-full flex flex-col overflow-hidden">
          <Editor
            code={code}
            onChange={setCode}
            activeLine={activeLine}
            errorLine={errorLine}
          />
        </div>

        {/* Right Side: Visualizers */}
        <div className="w-1/2 h-full flex flex-col bg-[#1e1e1e] border-l border-[#333333] overflow-hidden">
          {/* Tabs Bar */}
          <div role="tablist" aria-label="Compiler phase visualizers" className="flex items-center bg-[#252526] border-b border-[#333333] px-2 overflow-x-auto select-none shrink-0">
            {tabs.map((tab) => {
              const Icon = tab.icon;
              const isActive = activeTab === tab.id;
              return (
                <button
                  key={tab.id}
                  role="tab"
                  aria-selected={isActive}
                  onClick={() => setActiveTab(tab.id)}
                  className={`flex items-center space-x-2 px-3 py-2.5 text-xs font-medium border-b-2 transition whitespace-nowrap ${
                    isActive
                      ? 'border-blue-500 text-white bg-[#1e1e1e]'
                      : 'border-transparent text-gray-400 hover:text-gray-200 hover:bg-[#2d2d2d]'
                  }`}
                >
                  <Icon aria-hidden="true" className={`w-3.5 h-3.5 ${isActive ? 'text-blue-400' : 'text-gray-500'}`} />
                  <span>{tab.label}</span>
                  {tab.badge !== undefined && tab.badge !== '' && (
                    <span className={`text-[10px] px-1.5 py-0.2 rounded font-mono ${
                      isActive ? 'bg-blue-500/20 text-blue-400' : 'bg-[#333333] text-gray-400'
                    }`}
                    >
                      {tab.badge}
                    </span>
                  )}
                </button>
              );
            })}
          </div>

          {/* Active Visualization Panel Content */}
          <div className="flex-1 overflow-hidden relative" role="tabpanel">
            {activeTab === 'execution' && (
              <VMVisualizer
                vmTrace={compileResult?.vmTrace || []}
                onStepChange={setActiveLine}
              />
            )}
            {activeTab === 'tokens' && (
              <TokensPanel tokens={compileResult?.tokens || []} />
            )}
            {activeTab === 'ast' && (
              <ASTPanel ast={compileResult?.ast} />
            )}
            {activeTab === 'symbols' && (
              <SymbolTablePanel symbolTable={compileResult?.symbolTable || []} />
            )}
            {activeTab === 'tac' && (
              <TACPanel
                tac={compileResult?.tac || []}
                optTac={compileResult?.optTac || []}
                metrics={compileResult?.metrics}
              />
            )}
            {activeTab === 'bytecode' && (
              <BytecodePanel bytecode={compileResult?.bytecode || []} />
            )}
          </div>
        </div>
      </div>

      {/* Bottom Resizable Interactive Console Panel */}
      <ConsoleOutput
        consoleText={compileResult?.consoleOutput || ''}
        compileTimeMs={compileResult?.compile_time_ms}
        isSuccess={compileResult ? compileResult.success : true}
        hasResult={!!compileResult}
        diagnostics={compileResult?.diagnostics || []}
        onInputSubmit={handleTerminalInput}
        height={consoleHeight}
        onHeightChange={setConsoleHeight}
        terminalInputs={terminalInputs}
        waitingForInput={compileResult?.waitingForInput}
        inputPrompt={compileResult?.inputPrompt}
      />
    </div>
  );
}