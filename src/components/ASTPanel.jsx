import React, { useState } from 'react';
import { ChevronRight, ChevronDown, Cpu } from 'lucide-react';

function ASTNodeView({ node, depth = 0 }) {
  const [expanded, setExpanded] = useState(true);

  if (!node) return null;

  const children = node.children || [];
  const hasChildren = children.length > 0;

  return (
    <div className="ml-3 my-1">
      <div
        onClick={() => setExpanded(!expanded)}
        onKeyDown={(e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); setExpanded(!expanded); } }}
        role="button"
        tabIndex={0}
        aria-expanded={hasChildren ? expanded : undefined}
        className="flex items-center flex-wrap gap-x-2 gap-y-1 py-1 px-2 rounded hover:bg-[#2c2c2d] cursor-pointer text-xs font-mono border border-transparent hover:border-[#3c3c3c] transition"
      >
        {hasChildren ? (
          expanded ? (
            <ChevronDown aria-hidden="true" className="w-3.5 h-3.5 text-blue-400" />
          ) : (
            <ChevronRight aria-hidden="true" className="w-3.5 h-3.5 text-gray-500" />
          )
        ) : (
          <span className="w-3.5 h-3.5 inline-block" />
        )}

        <span className="bg-blue-500/20 text-blue-400 border border-blue-500/30 px-1.5 py-0.5 rounded text-[10px] font-bold">
          {node.type}
        </span>

        {node.identifier && (
          <span className="text-purple-300 font-semibold">{node.identifier}</span>
        )}
        {node.type_name && (
          <span className="text-teal-400 text-[11px] font-mono">({node.type_name})</span>
        )}
        {node.op && (
          <span className="text-yellow-400 font-bold px-1 bg-yellow-500/10 rounded">{node.op}</span>
        )}
        {node.num_val !== undefined && (
          <span className="text-amber-300 font-bold">{node.num_val}</span>
        )}
        {node.string_val !== undefined && (
          <span className="text-amber-300">"{node.string_val.replace(/\n/g, '\\n').replace(/\t/g, '\\t')}"</span>
        )}
        {node.is_array && (
          <span className="text-[9px] bg-cyan-900/40 text-cyan-300 border border-cyan-700/40 px-1 rounded">array</span>
        )}
        <span className="text-gray-500 text-[10px]">Line {node.line}</span>
      </div>

      {expanded && hasChildren && (
        <div className="border-l border-[#333333] pl-2">
          {children.map((child, idx) => (
            <ASTNodeView key={idx} node={child} depth={depth + 1} />
          ))}
        </div>
      )}
    </div>
  );
}

export default function ASTPanel({ ast }) {
  return (
    <div className="flex flex-col h-full bg-[#1e1e1e] p-4 font-mono text-xs overflow-auto">
      <div className="flex items-center justify-between mb-4 border-b border-[#333333] pb-2">
        <div className="flex items-center space-x-2">
          <Cpu aria-hidden="true" className="w-4 h-4 text-purple-400" />
          <span className="font-bold text-gray-200">Phase 02: Abstract Syntax Tree (AST)</span>
        </div>
        <span className="text-[11px] text-gray-400">Recursive-descent parse output</span>
      </div>

      <div className="flex-1 bg-[#252526] p-4 rounded-lg border border-[#333333] overflow-auto">
        {ast ? (
          <ASTNodeView node={ast} />
        ) : (
          <div className="text-gray-600 text-[11px]">Compile a program to see its syntax tree.</div>
        )}
      </div>
    </div>
  );
}
