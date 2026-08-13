import React from 'react';
import { Binary, Layers } from 'lucide-react';

export default function BytecodePanel({ bytecode }) {
  return (
    <div className="flex flex-col h-full bg-[#1e1e1e] p-4 text-xs font-mono">
      <div className="flex items-center justify-between mb-4 border-b border-[#333333] pb-2">
        <div className="flex items-center space-x-2">
          <Binary className="w-4 h-4 text-blue-400" />
          <span className="font-bold text-gray-200">Phase 06: Bytecode Generation</span>
        </div>
        <span className="text-[11px] text-gray-400">Targeting Stack-Based Virtual Machine</span>
      </div>

      {bytecode.length === 0 ? (
        <div className="flex-1 flex items-center justify-center text-gray-600 text-[11px]">
          Compile a program to generate bytecode.
        </div>
      ) : (
        <div className="flex-1 overflow-auto border border-[#333333] rounded-lg">
          <table className="w-full text-left border-collapse">
            <thead className="sticky top-0">
              <tr className="bg-[#252526] text-gray-400 border-b border-[#333333]">
                <th scope="col" className="p-2.5 border-r border-[#333333]">PC (Offset)</th>
                <th scope="col" className="p-2.5 border-r border-[#333333]">Opcode</th>
                <th scope="col" className="p-2.5 border-r border-[#333333]">Symbol / Target</th>
                <th scope="col" className="p-2.5 border-r border-[#333333]">Operand</th>
                <th scope="col" className="p-2.5">C Source Line</th>
              </tr>
            </thead>
            <tbody>
              {bytecode.map((instr, idx) => (
                <tr key={idx} className="border-b border-[#2a2a2a] hover:bg-[#252526] transition">
                  <td className="p-2.5 font-mono text-gray-500 border-r border-[#2a2a2a]">
                    0x{instr.pc.toString(16).padStart(4, '0').toUpperCase()}
                  </td>
                  <td className="p-2.5 border-r border-[#2a2a2a]">
                    <span className="bg-blue-500/20 text-blue-400 border border-blue-500/30 px-2 py-0.5 rounded font-bold">
                      {instr.op}
                    </span>
                  </td>
                  <td className="p-2.5 font-bold text-purple-300 border-r border-[#2a2a2a]">
                    {instr.symbol || '-'}
                  </td>
                  <td className="p-2.5 text-amber-400 border-r border-[#2a2a2a]">
                    {instr.operand}
                  </td>
                  <td className="p-2.5 text-gray-400">{instr.line > 0 ? `Line ${instr.line}` : '-'}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}
