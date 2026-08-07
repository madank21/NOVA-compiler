import React, { useState, useRef, useEffect } from 'react';
import { Terminal, CheckCircle2, Send, GripHorizontal, RotateCcw, HelpCircle, Loader2 } from 'lucide-react';

export default function ConsoleOutput({
  consoleText,
  compileTimeMs,
  isSuccess,
  onInputSubmit,
  height,
  onHeightChange,
  terminalInputs = [],
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
  }, [consoleText, terminalInputs, waitingForInput]);

  const handleMouseDown = (e) => {
    isDraggingRef.current = true;
    startYRef.current = e.clientY;
    startHeightRef.current = height;
    document.addEventListener('mousemove', handleMouseMove);
    document.addEventListener('mouseup', handleMouseUp);
  };

  const handleMouseMove = (e) => {
    if (!isDraggingRef.current) return;
    const deltaY = startYRef.current - e.clientY;
    const newHeight = Math.min(550, Math.max(120, startHeightRef.current + deltaY));
    onHeightChange(newHeight);
  };

  const handleMouseUp = () => {
    isDraggingRef.current = false;
    document.removeEventListener('mousemove', handleMouseMove);
    document.removeEventListener('mouseup', handleMouseUp);
  };

  const handleSendInput = (e) => {
    e.preventDefault();
    if (!inputText.trim()) return;

    const val = inputText.trim();
    setInputText('');

    if (onInputSubmit) {
      onInputSubmit(val);
    }
  };

  return (
    <div
      style={{ height: `${height}px` }}
      className="bg-[#181818] border-t border-[#333333] flex flex-col font-mono text-xs relative select-none"
    >
      {/* Resizable Drag Handle Bar */}
      <div
        onMouseDown={handleMouseDown}
        className="h-2 w-full bg-[#252526] border-b border-[#333333] hover:bg-blue-500/30 cursor-row-resize flex items-center justify-center transition group"
        title="Drag up/down to resize output area"
      >
        <GripHorizontal className="w-4 h-3 text-gray-500 group-hover:text-blue-400" />
      </div>

      {/* Console Header Bar */}
      <div className="bg-[#252526] px-4 py-1.5 border-b border-[#333333] flex items-center justify-between text-gray-400 select-none shrink-0">
        <div className="flex items-center space-x-2">
          <Terminal className="w-3.5 h-3.5 text-blue-400" />
          <span className="font-bold text-gray-200 text-[11px]">Interactive Terminal & Output</span>
          {waitingForInput ? (
            <span className="flex items-center space-x-1 bg-amber-500/20 text-amber-400 border border-amber-500/40 px-2 py-0.5 rounded text-[10px] font-bold animate-pulse">
              <Loader2 className="w-3 h-3 animate-spin" />
              <span>Waiting for User Input...</span>
            </span>
          ) : (
            <span className="text-[10px] bg-blue-500/20 text-blue-400 border border-blue-500/30 px-1.5 py-0.2 rounded font-mono">
              Dev-C++ / IDE Terminal Style
            </span>
          )}
        </div>
        <div className="flex items-center space-x-3 text-[11px]">
          {isSuccess && !waitingForInput && (
            <span className="flex items-center space-x-1 text-emerald-400">
              <CheckCircle2 className="w-3.5 h-3.5" />
              <span>Execution Finished ({compileTimeMs} ms)</span>
            </span>
          )}
        </div>
      </div>

      {/* Terminal Content Stream */}
      <div
        ref={consoleBottomRef}
        className="flex-1 p-3 overflow-auto text-gray-300 whitespace-pre-wrap selection:bg-blue-500/30 font-mono text-xs leading-5"
      >
        {consoleText || '[Console Ready — Click "Compile & Run" to execute code]'}
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
        <span className={`font-bold font-mono text-xs select-none ${
          waitingForInput ? 'text-amber-400' : 'text-blue-400'
        }`}>
          scanf &gt;&gt;
        </span>
        <input
          ref={inputRef}
          type="text"
          value={inputText}
          onChange={(e) => setInputText(e.target.value)}
          placeholder={
            inputPrompt
              ? `[Input Required]: ${inputPrompt} (press Enter to submit)`
              : 'Type input for scanf and press Enter...'
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
          <Send className="w-3 h-3" />
        </button>
      </form>
    </div>
  );
}
