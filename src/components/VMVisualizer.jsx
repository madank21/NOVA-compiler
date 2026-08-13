import React, { useState, useEffect } from 'react';
import { Play, Pause, SkipForward, SkipBack, RotateCcw, Cpu, HardDrive, Layers, Terminal } from 'lucide-react';

export default function VMVisualizer({ vmTrace, onStepChange }) {
  const [currentStepIdx, setCurrentStepIdx] = useState(0);
  const [isPlaying, setIsPlaying] = useState(false);
  const [speedMs, setSpeedMs] = useState(300);

  const totalSteps = vmTrace?.length || 0;
  const currentStep = vmTrace?.[currentStepIdx] || {
    pc: 0,
    line: 0,
    instruction: '',
    stack: [],
    variables: [],
    frames: [],
    console: ''
  };

  // clamp index when a new (shorter) trace arrives
  useEffect(() => {
    if (currentStepIdx >= totalSteps && totalSteps > 0) setCurrentStepIdx(totalSteps - 1);
    if (totalSteps === 0) setCurrentStepIdx(0);
  }, [totalSteps, currentStepIdx]);

  useEffect(() => {
    if (!isPlaying) return undefined;
    const timer = setInterval(() => {
      setCurrentStepIdx((prev) => {
        if (prev + 1 >= totalSteps) {
          setIsPlaying(false);
          return prev;
        }
        return prev + 1;
      });
    }, speedMs);
    return () => clearInterval(timer);
  }, [isPlaying, totalSteps, speedMs]);

  useEffect(() => {
    if (onStepChange && currentStep.line > 0) {
      onStepChange(currentStep.line);
    }
  }, [currentStepIdx, onStepChange, currentStep.line]);

  const handleReset = () => { setIsPlaying(false); setCurrentStepIdx(0); };
  const handleStepForward = () => { if (currentStepIdx + 1 < totalSteps) setCurrentStepIdx(currentStepIdx + 1); };
  const handleStepBack = () => { if (currentStepIdx > 0) setCurrentStepIdx(currentStepIdx - 1); };

  const fmtValue = (v) => (Number.isInteger(v) ? String(v) : String(v));
  const frames = currentStep.frames || [];

  return (
    <div className="flex flex-col h-full bg-[#1e1e1e] p-4 text-xs font-mono">
      {/* Playback Control Bar */}
      <div className="flex items-center justify-between mb-4 bg-[#252526] border border-[#333333] p-3 rounded-lg flex-wrap gap-2">
        <div className="flex items-center space-x-3">
          <button
            onClick={() => setIsPlaying(!isPlaying)}
            disabled={totalSteps === 0}
            aria-label={isPlaying ? 'Pause execution' : 'Play execution'}
            className="bg-blue-600 hover:bg-blue-500 disabled:opacity-40 text-white p-2 rounded-md transition shadow-md"
          >
            {isPlaying ? <Pause aria-hidden="true" className="w-4 h-4" /> : <Play aria-hidden="true" className="w-4 h-4 fill-current" />}
          </button>

          <button
            onClick={handleStepBack}
            disabled={currentStepIdx === 0}
            aria-label="Step backward"
            className="bg-[#2d2d2d] hover:bg-[#383838] text-gray-300 px-3 py-1.5 rounded border border-[#3c3c3c] transition disabled:opacity-40 flex items-center space-x-1"
          >
            <SkipBack aria-hidden="true" className="w-3.5 h-3.5" />
            <span>Back</span>
          </button>

          <button
            onClick={handleStepForward}
            disabled={currentStepIdx + 1 >= totalSteps}
            aria-label="Step forward"
            className="bg-[#2d2d2d] hover:bg-[#383838] text-gray-300 px-3 py-1.5 rounded border border-[#3c3c3c] transition disabled:opacity-40 flex items-center space-x-1"
          >
            <span>Step</span>
            <SkipForward aria-hidden="true" className="w-3.5 h-3.5" />
          </button>

          <button
            onClick={handleReset}
            aria-label="Reset to first step"
            className="bg-[#2d2d2d] hover:bg-[#383838] text-gray-400 p-1.5 rounded border border-[#3c3c3c] transition"
            title="Reset VM"
          >
            <RotateCcw aria-hidden="true" className="w-4 h-4" />
          </button>
        </div>

        {/* Speed Slider */}
        <div className="flex items-center space-x-3">
          <label htmlFor="vm-speed" className="text-gray-400 text-[11px]">Speed: {speedMs} ms</label>
          <input
            id="vm-speed"
            type="range"
            min="100"
            max="1000"
            step="100"
            value={speedMs}
            onChange={(e) => setSpeedMs(Number(e.target.value))}
            className="w-24 accent-blue-500 cursor-pointer"
          />
        </div>

        {/* Step Counter */}
        <div className="text-right" aria-live="polite">
          <span className="text-gray-400 text-[11px]">Execution step</span>
          <div className="text-blue-400 font-bold text-sm">
            {totalSteps === 0 ? '—' : `${currentStepIdx + 1} / ${totalSteps}`}
          </div>
        </div>
      </div>

      {totalSteps === 0 ? (
        <div className="flex-1 flex items-center justify-center text-gray-600 text-[11px]">
          Compile a program to step through its execution.
        </div>
      ) : (
        /* VM Grid Inspector (4 Columns) */
        <div className="flex-1 grid grid-cols-2 xl:grid-cols-4 gap-3 overflow-hidden">
          {/* Column 1: CPU Registers & PC */}
          <div className="flex flex-col bg-[#252526] border border-[#333333] rounded-lg p-3 overflow-auto">
            <div className="flex items-center space-x-2 border-b border-[#333333] pb-2 mb-3 text-purple-400 font-bold">
              <Cpu aria-hidden="true" className="w-4 h-4" />
              <span>CPU State</span>
            </div>

            <div className="space-y-3">
              <div className="bg-[#1e1e1e] p-2.5 rounded border border-[#333333]">
                <div className="text-[10px] text-gray-500 uppercase">Program Counter (PC)</div>
                <div className="text-base font-bold text-blue-400 font-mono">
                  0x{currentStep.pc.toString(16).padStart(4, '0').toUpperCase()}
                </div>
              </div>

              <div className="bg-[#1e1e1e] p-2.5 rounded border border-[#333333]">
                <div className="text-[10px] text-gray-500 uppercase">Executing Instruction</div>
                <div className="text-xs font-bold text-amber-300 font-mono mt-0.5 break-all">
                  {currentStep.instruction || '—'}
                </div>
                {currentStep.line > 0 && (
                  <div className="text-[10px] text-gray-400 mt-1">Source line: {currentStep.line}</div>
                )}
              </div>

              <div className="bg-[#1e1e1e] p-2.5 rounded border border-[#333333]">
                <div className="text-[10px] text-gray-500 uppercase flex items-center gap-1">
                  <Terminal aria-hidden="true" className="w-3 h-3" /> Console (so far)
                </div>
                <pre className="text-[10px] text-gray-300 whitespace-pre-wrap break-all mt-1 max-h-24 overflow-auto">{currentStep.console || ''}</pre>
              </div>
            </div>
          </div>

          {/* Column 2: Call Stack Activation Frames */}
          <div className="flex flex-col bg-[#252526] border border-[#333333] rounded-lg p-3 overflow-auto">
            <div className="flex items-center justify-between border-b border-[#333333] pb-2 mb-3 text-indigo-400 font-bold">
              <div className="flex items-center space-x-2">
                <Layers aria-hidden="true" className="w-4 h-4" />
                <span>Call Stack</span>
              </div>
              <span className="text-[10px] text-gray-500">{frames.length} frame{frames.length === 1 ? '' : 's'}</span>
            </div>

            <div className="space-y-2 overflow-auto flex-1">
              {frames.length === 0 ? (
                <div className="text-gray-600 text-center py-6 italic text-[11px]">[No frames]</div>
              ) : (
                [...frames].reverse().map((f, idx) => (
                  <div key={idx} className={`bg-[#1e1e1e] border p-2 rounded ${idx === 0 ? 'border-indigo-500/70' : 'border-indigo-500/30'}`}>
                    <div className="flex items-center justify-between">
                      <span className="text-indigo-300 font-bold">{f.func}</span>
                      <span className="text-[10px] bg-indigo-950 text-indigo-400 border border-indigo-700/40 px-1.5 py-0.2 rounded">
                        {idx === 0 ? 'active' : `frame ${frames.length - 1 - idx}`}
                      </span>
                    </div>
                    <div className="text-[10px] text-gray-500 mt-1">Return to: {f.retAddr}</div>
                  </div>
                ))
              )}
            </div>
          </div>

          {/* Column 3: Operand Stack */}
          <div className="flex flex-col bg-[#252526] border border-[#333333] rounded-lg p-3 overflow-hidden">
            <div className="flex items-center justify-between border-b border-[#333333] pb-2 mb-3 text-teal-400 font-bold">
              <div className="flex items-center space-x-2">
                <HardDrive aria-hidden="true" className="w-4 h-4" />
                <span>Operand Stack</span>
              </div>
              <span className="text-[10px] text-gray-500">Depth: {currentStep.stack.length}</span>
            </div>

            <div className="flex-1 overflow-auto flex flex-col-reverse gap-1.5 p-1">
              {currentStep.stack.length === 0 ? (
                <div className="text-gray-600 text-center py-6 italic text-[11px]">[Stack empty]</div>
              ) : (
                currentStep.stack.map((val, idx) => (
                  <div
                    key={idx}
                    className="bg-[#1e1e1e] border border-teal-500/30 text-teal-300 p-2 rounded font-bold font-mono flex items-center justify-between px-3"
                  >
                    <span className="text-[10px] text-gray-500">[{idx}]</span>
                    <span>{fmtValue(val)}</span>
                  </div>
                ))
              )}
            </div>
          </div>

          {/* Column 4: Variables & Memory Map */}
          <div className="flex flex-col bg-[#252526] border border-[#333333] rounded-lg p-3 overflow-auto">
            <div className="flex items-center space-x-2 border-b border-[#333333] pb-2 mb-3 text-amber-400 font-bold">
              <HardDrive aria-hidden="true" className="w-4 h-4" />
              <span>Variables &amp; Memory</span>
            </div>

            <div className="space-y-1.5 overflow-auto flex-1">
              {currentStep.variables.length === 0 ? (
                <div className="text-gray-600 text-center py-6 italic text-[11px]">[No variables]</div>
              ) : (
                currentStep.variables.map((v, idx) => (
                  <div
                    key={idx}
                    className="flex items-center justify-between bg-[#1e1e1e] border border-[#333333] px-2.5 py-1.5 rounded"
                  >
                    <span className="text-purple-300 font-semibold">{v.name}</span>
                    <span className="text-amber-400 font-bold">{fmtValue(v.value)}</span>
                  </div>
                ))
              )}
            </div>
          </div>
        </div>
      )}
    </div>
  );
}