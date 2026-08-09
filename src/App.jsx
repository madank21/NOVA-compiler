import React, { useState, useEffect } from 'react';
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
  const [activeLine, setActiveLine] = useState(null);
  const [consoleHeight, setConsoleHeight] = useState(200);
  const [terminalInputs, setTerminalInputs] = useState([]);

  useEffect(() => {
    handleCompile();
  }, []);

  const handleSelectPreset = (key) => {
    setSelectedPreset(key);
    if (PRESET_PROGRAMS[key]) {
      setCode(PRESET_PROGRAMS[key].code);
      setTerminalInputs([]);
      handleCompile(PRESET_PROGRAMS[key].code, []);
    }
  };

  const handleCompile = async (sourceToCompile = code, inputs = terminalInputs) => {
    setIsCompiling(true);

    if (backendMode === 'native') {
      try {
        const response = await fetch('http://localhost:8080/api/compile', {
          method: 'POST',
          headers: { 'Content-Type': 'text/plain' },
          body: sourceToCompile
        });
        const data = await response.json();
        setCompileResult(data);
      } catch (err) {
        console.warn('Native C server at 8080 unreachable, falling back to browser mirror engine', err);
        const result = compileCSource(sourceToCompile, inputs);
        setCompileResult(result);
      }
    } else {
      setTimeout(() => {
        const result = compileCSource(sourceToCompile, inputs);
        setCompileResult(result);
        setIsCompiling(false);
      }, 100);
      return;
    }

    setIsCompiling(false);
  };

  const handleTerminalInput = (inputVal) => {
    const updatedInputs = [...terminalInputs, inputVal];
    setTerminalInputs(updatedInputs);
    handleCompile(code, updatedInputs);
  };

  const handleFreshCompile = () => {
    setTerminalInputs([]);
    handleCompile(code, []);
  };

  const tabs = [
    { id: 'execution', label: '07. VM Execution Visualizer', icon: PlayCircle, badge: 'Phase 7' },
    { id: 'tokens', label: '01. Tokens', icon: Code, badge: compileResult?.tokens?.length },
    { id: 'ast', label: '02. AST Tree', icon: Layers, badge: 'Phase 2' },
    { id: 'symbols', label: '03. Symbol Table', icon: Database, badge: compileResult?.symbolTable?.length },
    { id: 'tac', label: '04. TAC & Optimization', icon: Zap, badge: `${compileResult?.metrics?.reduction_percentage || 31.4}%` },
    { id: 'bytecode', label: '05. Bytecode', icon: Binary, badge: compileResult?.bytecode?.length }
  ];

  return (
    <div className="flex flex-col h-screen bg-[#181818] text-[#cccccc] overflow-hidden">
      {/* Top Header */}
      <Header
        selectedPreset={selectedPreset}
        onSelectPreset={handleSelectPreset}
        onCompile={handleFreshCompile}
        isCompiling={isCompiling}
        backendMode={backendMode}
        setBackendMode={setBackendMode}
        compileTimeMs={compileResult?.compile_time_ms || 3.2}
      />

      {/* Main Studio Workspace */}
      <div className="flex-1 flex overflow-hidden">
        {/* Left Side: VS Code Editor */}
        <div className="w-1/2 h-full flex flex-col overflow-hidden">
          <Editor
            code={code}
            onChange={setCode}
            activeLine={activeLine}
          />
        </div>

        {/* Right Side: Visualizers */}
        <div className="w-1/2 h-full flex flex-col bg-[#1e1e1e] border-l border-[#333333] overflow-hidden">
          {/* Tabs Bar */}
          <div className="flex items-center bg-[#252526] border-b border-[#333333] px-2 overflow-x-auto select-none shrink-0">
            {tabs.map((tab) => {
              const Icon = tab.icon;
              const isActive = activeTab === tab.id;
              return (
                <button
                  key={tab.id}
                  onClick={() => setActiveTab(tab.id)}
                  className={`flex items-center space-x-2 px-3 py-2.5 text-xs font-medium border-b-2 transition whitespace-nowrap ${
                    isActive
                      ? 'border-blue-500 text-white bg-[#1e1e1e]'
                      : 'border-transparent text-gray-400 hover:text-gray-200 hover:bg-[#2d2d2d]'
                  }`}
                >
                  <Icon className={`w-3.5 h-3.5 ${isActive ? 'text-blue-400' : 'text-gray-500'}`} />
                  <span>{tab.label}</span>
                  {tab.badge && (
                    <span className={`text-[10px] px-1.5 py-0.2 rounded font-mono ${
                      isActive ? 'bg-blue-500/20 text-blue-400' : 'bg-[#333333] text-gray-400'
                    }`}>
                      {tab.badge}
                    </span>
                  )}
                </button>
              );
            })}
          </div>

          {/* Active Visualization Panel Content */}
          <div className="flex-1 overflow-hidden relative">
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
        compileTimeMs={compileResult?.compile_time_ms || 3.2}
        isSuccess={compileResult?.success !== false}
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
