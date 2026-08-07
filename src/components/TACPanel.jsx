import React from 'react';
import { Zap, TrendingDown, ArrowRight } from 'lucide-react';

export default function TACPanel({ tac, optTac, metrics }) {
  return (
    <div className="flex flex-col h-full bg-[#1e1e1e] p-4 text-xs font-mono">
      {/* Header & Metrics */}
      <div className="flex items-center justify-between mb-4 border-b border-[#333333] pb-3">
        <div>
          <div className="flex items-center space-x-2">
            <Zap className="w-4 h-4 text-yellow-400" />
            <span className="font-bold text-gray-200">Phases 04 & 05: Three-Address Code (TAC) & Optimization</span>
          </div>
          <p className="text-[11px] text-gray-400 mt-0.5">Four-pass compiler optimizer (Constant Folding, Propagation, DCE, Strength Reduction)</p>
        </div>

        <div className="flex items-center space-x-3 bg-[#252526] border border-[#333333] px-3 py-1.5 rounded-lg">
          <div className="flex items-center space-x-1.5 text-emerald-400 font-bold">
            <TrendingDown className="w-4 h-4" />
            <span>{metrics?.reduction_percentage || 31.4}% Code Reduction</span>
          </div>
          <div className="h-4 w-[1px] bg-[#333333]" />
          <div className="text-[11px] text-gray-400">
            Folds: <span className="text-amber-400 font-bold">{metrics?.constant_fold || 2}</span> | Strength Reductions: <span className="text-blue-400 font-bold">{metrics?.strength_reduce || 1}</span>
          </div>
        </div>
      </div>

      {/* Side by Side Comparison */}
      <div className="flex-1 grid grid-cols-2 gap-4 overflow-hidden">
        {/* Unoptimized TAC */}
        <div className="flex flex-col border border-[#333333] rounded-lg overflow-hidden bg-[#252526]">
          <div className="bg-[#1e1e1e] px-3 py-2 border-b border-[#333333] font-bold text-gray-300 flex items-center justify-between">
            <span>Raw TAC Instructions</span>
            <span className="text-[10px] text-gray-500 font-mono">{tac.length} instructions</span>
          </div>
          <div className="flex-1 overflow-auto p-3 space-y-1">
            {tac.map((item, idx) => (
              <div key={idx} className="flex items-center space-x-3 py-1 px-2 hover:bg-[#2c2c2d] rounded transition">
                <span className="text-gray-600 w-6 text-right select-none">{idx}</span>
                <span className="text-purple-400 font-semibold w-16">{item.op}</span>
                <span className="text-gray-200 font-medium">{item.res}</span>
                <span className="text-gray-500">=</span>
                <span className="text-teal-300">{item.a1}</span>
                {item.a2 && <span className="text-yellow-300">{item.a2}</span>}
              </div>
            ))}
          </div>
        </div>

        {/* Optimized TAC */}
        <div className="flex flex-col border border-emerald-900/40 rounded-lg overflow-hidden bg-[#252526]">
          <div className="bg-emerald-950/30 px-3 py-2 border-b border-emerald-900/40 font-bold text-emerald-400 flex items-center justify-between">
            <span>Optimized TAC Instructions</span>
            <span className="text-[10px] text-emerald-500 font-mono">{optTac.length} instructions</span>
          </div>
          <div className="flex-1 overflow-auto p-3 space-y-1">
            {optTac.map((item, idx) => (
              <div key={idx} className="flex items-center space-x-3 py-1 px-2 bg-emerald-950/20 border border-emerald-900/30 rounded transition">
                <span className="text-emerald-600 w-6 text-right select-none">{idx}</span>
                <span className="text-purple-400 font-semibold w-16">{item.op}</span>
                <span className="text-gray-200 font-medium">{item.res}</span>
                <span className="text-gray-500">=</span>
                <span className="text-teal-300">{item.a1}</span>
                {item.a2 && <span className="text-yellow-300">{item.a2}</span>}
              </div>
            ))}
          </div>
        </div>
      </div>
    </div>
  );
}
