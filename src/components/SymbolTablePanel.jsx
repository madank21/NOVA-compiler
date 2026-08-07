import React from 'react';
import { Database, ShieldCheck } from 'lucide-react';

export default function SymbolTablePanel({ symbolTable }) {
  return (
    <div className="flex flex-col h-full bg-[#1e1e1e] p-4 text-xs font-mono">
      <div className="flex items-center justify-between mb-4 border-b border-[#333333] pb-2">
        <div className="flex items-center space-x-2">
          <Database className="w-4 h-4 text-teal-400" />
          <span className="font-bold text-gray-200">Phase 03: Semantic Analysis & Symbol Table</span>
        </div>
        <div className="flex items-center space-x-1.5 text-emerald-400 bg-emerald-950/40 border border-emerald-800/40 px-2 py-0.5 rounded text-[11px]">
          <ShieldCheck className="w-3.5 h-3.5" />
          <span>Scope & Type Checks Passed</span>
        </div>
      </div>

      <div className="flex-1 overflow-auto border border-[#333333] rounded-lg">
        <table className="w-full text-left border-collapse">
          <thead>
            <tr className="bg-[#252526] text-gray-400 border-b border-[#333333]">
              <th className="p-2.5 border-r border-[#333333]">Scope</th>
              <th className="p-2.5 border-r border-[#333333]">Symbol Name</th>
              <th className="p-2.5 border-r border-[#333333]">Symbol Kind</th>
              <th className="p-2.5 border-r border-[#333333]">Type</th>
              <th className="p-2.5 border-r border-[#333333]">Memory Address</th>
              <th className="p-2.5">Params</th>
            </tr>
          </thead>
          <tbody>
            {symbolTable.map((sym, idx) => (
              <tr key={idx} className="border-b border-[#2a2a2a] hover:bg-[#252526] transition">
                <td className="p-2.5 border-r border-[#2a2a2a]">
                  <span className="bg-purple-500/20 text-purple-400 border border-purple-500/30 px-2 py-0.5 rounded text-[11px]">
                    {sym.scope}
                  </span>
                </td>
                <td className="p-2.5 font-bold text-gray-100 border-r border-[#2a2a2a]">{sym.name}</td>
                <td className="p-2.5 border-r border-[#2a2a2a] text-blue-400 font-medium">{sym.kind}</td>
                <td className="p-2.5 border-r border-[#2a2a2a] text-teal-400 font-semibold">{sym.type}</td>
                <td className="p-2.5 border-r border-[#2a2a2a] text-amber-400">{sym.address}</td>
                <td className="p-2.5 text-gray-400">{sym.params}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}
