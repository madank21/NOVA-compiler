import React from 'react';
import { Play, Code2, RefreshCw, Server, MonitorSmartphone } from 'lucide-react';
import { PRESET_PROGRAMS } from '../engine/presets';

export default function Header({
  selectedPreset,
  onSelectPreset,
  onCompile,
  isCompiling,
  backendMode,
  setBackendMode,
  activeEngine,
  compileTimeMs
}) {
  return (
    <header className="bg-[#1e1e1e] border-b border-[#333333] px-4 py-2.5 flex items-center justify-between shadow-lg">
      <div className="flex items-center space-x-3">
        <div className="w-8 h-8 rounded-lg bg-gradient-to-tr from-blue-600 via-indigo-500 to-teal-400 p-[1px] flex items-center justify-center shadow-md">
          <div className="w-full h-full bg-[#1e1e1e] rounded-[7px] flex items-center justify-center">
            <Code2 aria-hidden="true" className="w-4 h-4 text-blue-400" />
          </div>
        </div>
        <div>
          <div className="flex items-center space-x-2">
            <h1 className="font-bold text-white text-base tracking-wide flex items-center gap-2">
              NOVA STUDIO
              <span className="text-[10px] bg-blue-500/20 text-blue-400 border border-blue-500/30 px-1.5 py-0.5 rounded font-mono">
                C COMPILER v2.0
              </span>
            </h1>
          </div>
          <p className="text-[11px] text-gray-400">
            C subset compiler pipeline &amp; stack-VM visualizer
            {typeof compileTimeMs === 'number' && (
              <span className="text-gray-500"> · last compile {compileTimeMs} ms</span>
            )}
          </p>
        </div>
      </div>

      <div className="flex items-center space-x-3">
        {/* Engine badge: which engine actually produced the current result */}
        {activeEngine && (
          <span
            className={`flex items-center space-x-1 text-[10px] px-2 py-1 rounded border font-mono ${
              activeEngine === 'native'
                ? 'bg-emerald-950/40 text-emerald-300 border-emerald-700/50'
                : 'bg-sky-950/40 text-sky-300 border-sky-700/50'
            }`}
            title="Engine that produced the current compiler output"
          >
            {activeEngine === 'native'
              ? <Server aria-hidden="true" className="w-3 h-3" />
              : <MonitorSmartphone aria-hidden="true" className="w-3 h-3" />}
            <span>{activeEngine === 'native' ? 'pure C backend' : 'browser engine'}</span>
          </span>
        )}

        {/* Preset Selector */}
        <label className="sr-only" htmlFor="preset-select">Example program</label>
        <select
          id="preset-select"
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
          aria-pressed={backendMode === 'native'}
          className={`text-xs px-2.5 py-1.5 rounded-md border flex items-center space-x-1.5 transition ${
            backendMode === 'native'
              ? 'bg-emerald-950/40 text-emerald-400 border-emerald-700/50'
              : 'bg-[#252526] text-gray-300 border-[#3c3c3c]'
          }`}
          title="Toggle between the native pure-C backend server and the in-browser engine"
        >
          <Server aria-hidden="true" className="w-3.5 h-3.5" />
          <span>{backendMode === 'native' ? 'Native C backend' : 'In-browser engine'}</span>
        </button>

        {/* Compile & Run Button */}
        <button
          onClick={onCompile}
          disabled={isCompiling}
          className="bg-blue-600 hover:bg-blue-500 text-white font-medium text-xs px-4 py-1.5 rounded-md flex items-center space-x-2 transition shadow-md disabled:opacity-50"
        >
          {isCompiling ? (
            <RefreshCw aria-hidden="true" className="w-3.5 h-3.5 animate-spin" />
          ) : (
            <Play aria-hidden="true" className="w-3.5 h-3.5 fill-current" />
          )}
          <span>{isCompiling ? 'Compiling…' : 'Compile & Run'}</span>
        </button>
      </div>
    </header>
  );
}