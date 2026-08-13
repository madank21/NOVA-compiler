import React, { useState, useRef, useEffect } from 'react';
import { Terminal, CheckCircle2, XCircle, Send, GripHorizontal, Loader2, Clock } from 'lucide-react';

export default function ConsoleOutput({
  consoleText,
  compileTimeMs,
  isSuccess,
  hasResult,
  diagnostics = [],
  onInputSubmit,
  height,
  onHeightChange,
  waitingForInput = false,
  inputPrompt = ''
}) {
  const [inputText, setInputText] = useState('');
  const inputRef = useRef(null);
  const isDraggingRef = useRef(false);
  const startYRef = useRef(0);
  const startHeightRef = useRef(0);
  const consoleBottomRef = useRef(null);

  useEffect(() => {
    if (consoleBottomRef.current) {
      consoleBottomRef.current.scrollTop = consoleBottomRef.current.scrollHeight;
    }
    if (waitingForInput && inputRef.current) {
      inputRef.current.focus();
    }
  }, [consoleText, waitingForInput]);

  const handleMouseDown = (e) => {
    isDraggingRef.current = true;
    startYRef.current = e.clientY;
    startHeightRef.current = height;
    const move = (ev) => {
      if (!isDraggingRef.current) return;
      const deltaY = startYRef.current - ev.clientY;
      const newHeight = Math.min(550, Math.max(120, startHeightRef.current + deltaY));
      onHeightChange(newHeight);
    };
    const up = () => {
      isDraggingRef.current = false;
      document.removeEventListener('mousemove', move);
      document.removeEventListener('mouseup', up);
    };
    document.addEventListener('mousemove', move);
    document.addEventListener('mouseup', up);
  };

  const handleSendInput = (e) => {
    e.preventDefault();
    if (!inputText.trim()) return;
    const val = inputText.trim();
    setInputText('');
    if (onInputSubmit) onInputSubmit(val);
  };

  const diagColor = (level) => {
    if (level === 'error') return 'text-rose-300';
    if (level === 'runtime') return 'text-orange-300';
    return 'text-yellow-300';
  };

  return (
    <div
      style={{ height: `${height}px` }}
      className="bg-[#181818] border-t border-[#333333] flex flex-col font-mono text-xs relative"
    >
      {/* Resizable Drag Handle Bar */}
      <div
        onMouseDown={handleMouseDown}
        className="h-2 w-full bg-[#252526] border-b border-[#333333] hover:bg-blue-500/30 cursor-row-resize flex items-center justify-center transition group"
        title="Drag up/down to resize the output area"
      >
        <GripHorizontal aria-hidden="true" className="w-4 h-3 text-gray-500 group-hover:text-blue-400" />
      </div>

      {/* Console Header Bar */}
      <div className="bg-[#252526] px-4 py-1.5 border-b border-[#333333] flex items-center justify-between text-gray-400 select-none shrink-0">
        <div className="flex items-center space-x-2">
          <Terminal aria-hidden="true" className="w-3.5 h-3.5 text-blue-400" />
          <span className="font-bold text-gray-200 text-[11px]">Program Output &amp; Interactive Terminal</span>
          {waitingForInput && (
            <span className="flex items-center space-x-1 bg-amber-500/20 text-amber-400 border border-amber-500/40 px-2 py-0.5 rounded text-[10px] font-bold animate-pulse">
              <Loader2 aria-hidden="true" className="w-3 h-3 animate-spin" />
              <span>Waiting for input…</span>
            </span>
          )}
        </div>
        <div className="flex items-center space-x-3 text-[11px]" role="status" aria-live="polite">
          {!hasResult ? (
            <span className="flex items-center space-x-1 text-gray-400">
              <Clock aria-hidden="true" className="w-3.5 h-3.5" />
              <span>Ready</span>
            </span>
          ) : isSuccess && !waitingForInput ? (
            <span className="flex items-center space-x-1 text-emerald-400">
              <CheckCircle2 aria-hidden="true" className="w-3.5 h-3.5" />
              <span>Execution finished{typeof compileTimeMs === 'number' ? ` (${compileTimeMs} ms)` : ''}</span>
            </span>
          ) : !waitingForInput ? (
            <span className="flex items-center space-x-1 text-rose-400">
              <XCircle aria-hidden="true" className="w-3.5 h-3.5" />
              <span>Compilation failed{typeof compileTimeMs === 'number' ? ` (${compileTimeMs} ms)` : ''}</span>
            </span>
          ) : null}
        </div>
      </div>

      {/* Terminal Content Stream — text is selectable so output can be copied */}
      <div
        ref={consoleBottomRef}
        className="flex-1 p-3 overflow-auto text-gray-300 whitespace-pre-wrap select-text selection:bg-blue-500/30 font-mono text-xs leading-5"
      >
        {diagnostics.length > 0 && (
          <div className="mb-3 rounded border border-rose-500/30 bg-rose-950/30 p-3 text-rose-200">
            <div className="text-[11px] uppercase tracking-[0.2em] text-rose-300 font-bold mb-2">Diagnostics</div>
            {diagnostics.map((diag, idx) => (
              <div key={idx} className="mb-1 text-[12px]">
                <span className={`font-semibold ${diagColor(diag.level)}`}>{diag.level.toUpperCase()}</span>
                {diag.line > 0 && <span className="text-rose-400/70"> (line {diag.line}{diag.column > 0 ? `, col ${diag.column}` : ''})</span>}
                : {diag.msg}
              </div>
            ))}
          </div>
        )}

        {consoleText || (diagnostics.length === 0 ? '[No output — compile a program to run it]' : '')}
      </div>

      {/* Interactive Input Prompt (scanf / stdin) */}
      <form
        onSubmit={handleSendInput}
        className={`px-3 py-2 flex items-center space-x-2 shrink-0 transition ${
          waitingForInput
            ? 'bg-amber-950/30 border-t border-amber-600/50'
            : 'bg-[#1e1e1e] border-t border-[#333333]'
        }`}
      >
        <label htmlFor="scanf-input" className={`font-bold font-mono text-xs select-none ${
          waitingForInput ? 'text-amber-400' : 'text-blue-400'
        }`}
        >
          scanf &gt;&gt;
        </label>
        <input
          id="scanf-input"
          ref={inputRef}
          type="text"
          value={inputText}
          onChange={(e) => setInputText(e.target.value)}
          placeholder={
            inputPrompt
              ? `${inputPrompt} — press Enter to submit`
              : 'Type input for scanf and press Enter…'
          }
          className={`flex-1 bg-[#252526] border rounded px-3 py-1.5 text-xs text-gray-200 font-mono focus:outline-none transition ${
            waitingForInput
              ? 'border-amber-500/70 focus:border-amber-400 ring-1 ring-amber-500/30'
              : 'border-[#3c3c3c] focus:border-blue-500'
          }`}
        />
        <button
          type="submit"
          className={`px-4 py-1.5 rounded flex items-center space-x-1 text-xs font-bold transition shadow ${
            waitingForInput
              ? 'bg-amber-600 hover:bg-amber-500 text-white animate-pulse'
              : 'bg-blue-600 hover:bg-blue-500 text-white'
          }`}
        >
          <span>Send Input</span>
          <Send aria-hidden="true" className="w-3 h-3" />
        </button>
      </form>
    </div>
  );
}