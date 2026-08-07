import React from 'react';
import { Play, Cpu, Code2, Layers, RefreshCw, Zap, Server } from 'lucide-react';
import { PRESET_PROGRAMS } from '../engine/presets';

export default function Header({
  selectedPreset,
  onSelectPreset,
  onCompile,
  isCompiling,
  backendMode,
  setBackendMode,
  compileTimeMs
}) {
  return (
    <header className="bg-[#1e1e1e] border-b border-[#333333] px-4 py-2.5 flex items-center justify-between shadow-lg">
      <div className="flex items-center space-x-3">
        <div className="w-8 h-8 rounded-lg bg-gradient-to-tr from-blue-600 via-indigo-500 to-teal-400 p-[1px] flex items-center justify-center shadow-md">
          <div className="w-full h-full bg-[#1e1e1e] rounded-[7px] flex items-center justify-center">
            <Code2 className="w-4 h-4 text-blue-400" />
          </div>
        </div>
        <div>
          <div className="flex items-center space-x-2">
            <h1 className="font-bold text-white text-base tracking-wide flex items-center gap-2">
              NOVA STUDIO
              <span className="text-[10px] bg-blue-500/20 text-blue-400 border border-blue-500/30 px-1.5 py-0.5 rounded font-mono">
                C COMPILER v1.0
              </span>
            </h1>
          </div>
          <p className="text-[11px] text-gray-400">Pure C Compiler Pipeline & Stack VM Execution Visualizer</p>
        </div>
      </div>

      <div className="flex items-center space-x-3">
        {/* Preset Selector */}
        <select
          value={selectedPreset}
          onChange={(e) => onSelectPreset(e.target.value)}
          className="bg-[#252526] border border-[#3c3c3c] text-xs text-gray-200 rounded-md px-3 py-1.5 focus:outline-none focus:border-blue-500 hover:bg-[#2d2d2d] transition"
        >
          {Object.entries(PRESET_PROGRAMS).map(([key, item]) => (
            <option key={key} value={key}>
              {item.name}
            </option>
          ))}
        </select>

        {/* Backend Mode Switcher */}
        <button
          onClick={() => setBackendMode(backendMode === 'native' ? 'browser' : 'native')}
          className={`text-xs px-2.5 py-1.5 rounded-md border flex items-center space-x-1.5 transition ${
            backendMode === 'native'
              ? 'bg-emerald-950/40 text-emerald-400 border-emerald-700/50'
              : 'bg-[#252526] text-gray-300 border-[#3c3c3c]'
          }`}
          title="Toggle between Native Pure C Backend Server & Browser Engine"
        >
          <Server className="w-3.5 h-3.5" />
          <span>{backendMode === 'native' ? 'Pure C Server (8080)' : 'In-Browser Engine'}</span>
        </button>

        {/* Compile & Run Button */}
        <button
          onClick={onCompile}
          disabled={isCompiling}
          className="bg-blue-600 hover:bg-blue-500 text-white font-medium text-xs px-4 py-1.5 rounded-md flex items-center space-x-2 transition shadow-md disabled:opacity-50"
        >
          {isCompiling ? (
            <RefreshCw className="w-3.5 h-3.5 animate-spin" />
          ) : (
            <Play className="w-3.5 h-3.5 fill-current" />
          )}
          <span>{isCompiling ? 'Compiling...' : 'Compile & Run'}</span>
        </button>
      </div>
    </header>
  );
}
