import React from 'react';
import { Zap, TrendingDown } from 'lucide-react';

// Renders one TAC row according to its opcode shape.
function TacRow({ item, idx, tone }) {
  const idxColor = tone === 'opt' ? 'text-emerald-600' : 'text-gray-600';
  const rowBg = tone === 'opt' ? 'bg-emerald-950/20 border border-emerald-900/30' : '';
  const { op, res, a1, a2 } = item;

  let body;
  if (op === 'LABEL') {
    body = <><span className="text-pink-400 font-semibold">{res}:</span></>;
  } else if (op === 'GOTO') {
    body = <><span className="text-pink-400 font-semibold">goto</span> <span className="text-pink-300">{res}</span></>;
  } else if (op === 'IF_FALSE') {
    body = <><span className="text-pink-400 font-semibold">ifFalse</span> <span className="text-teal-300">{a1}</span> <span className="text-gray-500">→</span> <span className="text-pink-300">{res}</span></>;
  } else if (op === 'PARAM') {
    body = <><span className="text-pink-400 font-semibold">param</span> <span className="text-teal-300">{a1}</span></>;
  } else if (op === 'CALL') {
    body = <><span className="text-gray-200">{res}</span> <span className="text-gray-500">=</span> <span className="text-pink-400 font-semibold">call</span> <span className="text-purple-300">{a1}</span> <span className="text-gray-500">({a2})</span></>;
  } else if (op === 'RETURN') {
    body = <><span className="text-pink-400 font-semibold">return</span> {a1 && <span className="text-teal-300">{a1}</span>}</>;
  } else if (op === 'FUNC_BEGIN' || op === 'FUNC_END') {
    body = <span className="text-blue-300 font-semibold">{op === 'FUNC_BEGIN' ? 'func' : 'endfunc'} <span className="text-purple-300">{res}</span></span>;
  } else if (op === 'PRINT' || op === 'READ') {
    body = <><span className="text-pink-400 font-semibold">{op.toLowerCase()}</span> <span className="text-amber-300">{a1}</span> <span className="text-gray-500">({a2} args)</span></>;
  } else if (op === 'ADDR' || op === 'IDX_ADDR' || op === 'LOAD_PTR' || op === 'STORE_PTR') {
    body = <><span className="text-gray-200">{res && `${res} `}</span><span className="text-gray-500">=</span> <span className="text-purple-400 font-semibold">{op}</span> <span className="text-teal-300">{a1}</span>{a2 ? <span className="text-yellow-300"> {a2}</span> : null}</>;
  } else if (op === '=') {
    body = <><span className="text-gray-200">{res}</span> <span className="text-gray-500">=</span> <span className="text-teal-300">{a1}</span></>;
  } else if (op === 'neg' || op === '!') {
    body = <><span className="text-gray-200">{res}</span> <span className="text-gray-500">=</span> <span className="text-purple-400 font-semibold">{op}</span> <span className="text-teal-300">{a1}</span></>;
  } else {
    // binary: res = a1 op a2
    body = <><span className="text-gray-200">{res}</span> <span className="text-gray-500">=</span> <span className="text-teal-300">{a1}</span> <span className="text-yellow-300 font-semibold">{op}</span> <span className="text-teal-300">{a2}</span></>;
  }

  return (
    <div className={`flex items-center space-x-3 py-1 px-2 rounded transition ${rowBg} ${tone !== 'opt' ? 'hover:bg-[#2c2c2d]' : ''}`}>
      <span className={`${idxColor} w-6 text-right select-none`}>{idx}</span>
      <span className="font-mono">{body}</span>
    </div>
  );
}

export default function TACPanel({ tac, optTac, metrics }) {
  const m = metrics || { constant_fold: 0, constant_prop: 0, dead_code: 0, strength_reduce: 0, reduction_percentage: 0 };

  return (
    <div className="flex flex-col h-full bg-[#1e1e1e] p-4 text-xs font-mono">
      {/* Header & real metrics */}
      <div className="flex items-center justify-between mb-4 border-b border-[#333333] pb-3">
        <div>
          <div className="flex items-center space-x-2">
            <Zap aria-hidden="true" className="w-4 h-4 text-yellow-400" />
            <span className="font-bold text-gray-200">Phases 04 &amp; 05: Three-Address Code (TAC) &amp; Optimization</span>
          </div>
          <p className="text-[11px] text-gray-400 mt-0.5">Constant folding · constant propagation · strength reduction · dead-code elimination</p>
        </div>

        <div className="flex items-center space-x-3 bg-[#252526] border border-[#333333] px-3 py-1.5 rounded-lg">
          <div className="flex items-center space-x-1.5 text-emerald-400 font-bold">
            <TrendingDown aria-hidden="true" className="w-4 h-4" />
            <span>{m.reduction_percentage}% fewer instructions</span>
          </div>
          <div className="h-4 w-[1px] bg-[#333333]" />
          <div className="text-[11px] text-gray-400">
            folds: <span className="text-amber-400 font-bold">{m.constant_fold}</span> ·
            props: <span className="text-sky-400 font-bold"> {m.constant_prop}</span> ·
            strength: <span className="text-blue-400 font-bold"> {m.strength_reduce}</span> ·
            DCE: <span className="text-rose-400 font-bold"> {m.dead_code}</span>
          </div>
        </div>
      </div>

      {/* Side by Side Comparison */}
      <div className="flex-1 grid grid-cols-2 gap-4 overflow-hidden">
        <div className="flex flex-col border border-[#333333] rounded-lg overflow-hidden bg-[#252526]">
          <div className="bg-[#1e1e1e] px-3 py-2 border-b border-[#333333] font-bold text-gray-300 flex items-center justify-between">
            <span>Raw TAC</span>
            <span className="text-[10px] text-gray-500 font-mono">{tac.length} instructions</span>
          </div>
          <div className="flex-1 overflow-auto p-3 space-y-1">
            {tac.map((item, idx) => <TacRow key={idx} item={item} idx={idx} tone="raw" />)}
          </div>
        </div>

        <div className="flex flex-col border border-emerald-900/40 rounded-lg overflow-hidden bg-[#252526]">
          <div className="bg-emerald-950/30 px-3 py-2 border-b border-emerald-900/40 font-bold text-emerald-300 flex items-center justify-between">
            <span>Optimized TAC</span>
            <span className="text-[10px] text-emerald-500 font-mono">{optTac.length} instructions</span>
          </div>
          <div className="flex-1 overflow-auto p-3 space-y-1">
            {optTac.map((item, idx) => <TacRow key={idx} item={item} idx={idx} tone="opt" />)}
          </div>
        </div>
      </div>
    </div>
  );
}
