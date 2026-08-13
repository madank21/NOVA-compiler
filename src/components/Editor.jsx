import React, { useRef } from 'react';

const TYPES = new Set(['int', 'float', 'char', 'void', 'double', 'long', 'short', 'struct']);
const KEYWORDS = new Set([
  'if', 'else', 'while', 'for', 'return', 'break', 'continue', 'switch', 'case',
  'default', 'sizeof', 'do', 'goto', 'typedef', 'enum', 'union', 'const', 'static'
]);
const BUILTINS = new Set(['printf', 'scanf']);

export default function Editor({ code, onChange, activeLine, errorLine }) {
  const lines = code.split('\n');
  const textareaRef = useRef(null);
  const lineNumbersRef = useRef(null);
  const highlightRef = useRef(null);

  const handleScroll = () => {
    if (textareaRef.current) {
      const top = textareaRef.current.scrollTop;
      const left = textareaRef.current.scrollLeft;
      if (lineNumbersRef.current) lineNumbersRef.current.scrollTop = top;
      if (highlightRef.current) {
        highlightRef.current.scrollTop = top;
        highlightRef.current.scrollLeft = left;
      }
    }
  };

  const handleKeyDown = (e) => {
    if (e.key === 'Tab') {
      e.preventDefault();
      const ta = e.currentTarget;
      const start = ta.selectionStart;
      const end = ta.selectionEnd;
      const next = code.slice(0, start) + '    ' + code.slice(end);
      onChange(next);
      requestAnimationFrame(() => {
        ta.selectionStart = ta.selectionEnd = start + 4;
      });
    }
  };

  const renderSyntaxLine = (lineText) => {
    if (!lineText) return <span>&nbsp;</span>;

    const parts = lineText.split(/(\s+|#include|#define|<[^>]+>|".*?"|'.*?'|\/\/.*)/);

    return parts.map((part, idx) => {
      if (!part) return null;

      if (part.startsWith('//')) {
        return <span key={idx} className="syntax-comment">{part}</span>;
      }
      if (part.startsWith('#include') || part.startsWith('#define')) {
        return <span key={idx} className="syntax-preprocessor">{part}</span>;
      }
      if (part.startsWith('<') && part.endsWith('>')) {
        return <span key={idx} className="syntax-string">{part}</span>;
      }
      if (part.startsWith('"') || part.startsWith("'")) {
        return <span key={idx} className="syntax-string">{part}</span>;
      }

      // words; an identifier immediately followed by '(' is a call target
      const words = part.split(/([a-zA-Z_][a-zA-Z0-9_]*|\d+\.?\d*)/);
      return words.map((w, wIdx) => {
        if (!w) return null;
        if (/^\d/.test(w)) {
          return <span key={wIdx} className="syntax-number">{w}</span>;
        }
        if (/^[a-zA-Z_]/.test(w)) {
          const rest = part.slice(part.indexOf(w, words.slice(0, wIdx).join('').length) + w.length);
          const isCall = /^\s*\(/.test(rest) || BUILTINS.has(w);
          if (TYPES.has(w)) return <span key={wIdx} className="syntax-type">{w}</span>;
          if (KEYWORDS.has(w)) return <span key={wIdx} className="syntax-keyword">{w}</span>;
          if (isCall) return <span key={wIdx} className="syntax-function">{w}</span>;
          return <span key={wIdx} className="syntax-variable">{w}</span>;
        }
        return <span key={wIdx} className="syntax-operator">{w}</span>;
      });
    });
  };

  const sharedTextStyle = { tabSize: 4 };

  return (
    <div className="flex flex-col h-full bg-[#1e1e1e] border-r border-[#333333] overflow-hidden">
      {/* File Header */}
      <div className="bg-[#252526] px-4 py-1.5 border-b border-[#333333] flex items-center justify-between text-xs text-gray-400 select-none">
        <div className="flex items-center space-x-2">
          <span className="w-2.5 h-2.5 rounded-full bg-blue-500 inline-block" aria-hidden="true"></span>
          <span className="font-mono text-gray-200 font-medium">main.c</span>
          <span className="text-[10px] text-gray-500 font-mono">(NOVA C subset)</span>
        </div>
        <div className="text-[11px] text-gray-500 font-mono">
          {lines.length} lines
        </div>
      </div>

      {/* Editor Body */}
      <div className="flex-1 relative flex overflow-hidden font-mono text-xs leading-6">
        {/* Line Numbers Sidebar */}
        <div
          ref={lineNumbersRef}
          aria-hidden="true"
          className="w-14 bg-[#1e1e1e] border-r border-[#2d2d2d] py-3 text-right pr-3 text-gray-600 select-none overflow-hidden shrink-0"
        >
          {lines.map((_, i) => {
            const lineNum = i + 1;
            const isActive = activeLine === lineNum;
            const isError = errorLine === lineNum;
            return (
              <div
                key={i}
                className={`h-6 ${
                  isError
                    ? 'text-red-400 font-bold bg-red-500/20'
                    : isActive
                    ? 'text-blue-400 font-bold bg-blue-500/10'
                    : ''
                }`}
              >
                {lineNum}
              </div>
            );
          })}
        </div>

        {/* Textarea & Syntax Overlay Container */}
        <div className="flex-1 relative overflow-hidden">
          {/* Highlight Layer — identical padding/font metrics to the textarea */}
          <div
            ref={highlightRef}
            aria-hidden="true"
            style={sharedTextStyle}
            className="absolute inset-0 p-3 overflow-hidden font-mono text-xs leading-6 pointer-events-none whitespace-pre"
          >
            {lines.map((line, i) => {
              const lineNum = i + 1;
              const isActive = activeLine === lineNum;
              const isError = errorLine === lineNum;
              return (
                <div
                  key={i}
                  className={`h-6 rounded ${
                    isError
                      ? 'bg-red-950/40 border-l-2 border-red-500'
                      : isActive
                      ? 'bg-[#2c2c2d] border-l-2 border-blue-500'
                      : ''
                  }`}
                >
                  {renderSyntaxLine(line)}
                </div>
              );
            })}
          </div>

          {/* User Input Textarea */}
          <textarea
            ref={textareaRef}
            value={code}
            onChange={(e) => onChange(e.target.value)}
            onScroll={handleScroll}
            onKeyDown={handleKeyDown}
            spellCheck={false}
            aria-label="C source code editor"
            style={sharedTextStyle}
            className="absolute inset-0 w-full h-full p-3 bg-transparent text-transparent caret-blue-400 font-mono text-xs leading-6 resize-none focus:outline-none z-10 whitespace-pre overflow-auto"
          />
        </div>
      </div>
    </div>
  );
}