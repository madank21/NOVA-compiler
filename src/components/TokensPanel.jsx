import React, { useState } from 'react';
import { Search } from 'lucide-react';

const KEYWORD_TYPES = new Set([
  'TOKEN_AUTO', 'TOKEN_BREAK', 'TOKEN_CASE', 'TOKEN_CONST', 'TOKEN_CONTINUE',
  'TOKEN_DEFAULT', 'TOKEN_DO', 'TOKEN_ELSE', 'TOKEN_ENUM', 'TOKEN_EXTERN',
  'TOKEN_FOR', 'TOKEN_GOTO', 'TOKEN_IF', 'TOKEN_LONG', 'TOKEN_REGISTER',
  'TOKEN_RETURN', 'TOKEN_SHORT', 'TOKEN_SIGNED', 'TOKEN_SIZEOF', 'TOKEN_STATIC',
  'TOKEN_STRUCT', 'TOKEN_SWITCH', 'TOKEN_TYPEDEF', 'TOKEN_UNION', 'TOKEN_UNSIGNED',
  'TOKEN_VOLATILE', 'TOKEN_WHILE', 'TOKEN_VOID'
]);
const TYPE_TOKENS = new Set(['TOKEN_INT', 'TOKEN_FLOAT', 'TOKEN_DOUBLE', 'TOKEN_CHAR']);

export default function TokensPanel({ tokens }) {
  const [filter, setFilter] = useState('');

  const filteredTokens = tokens.filter(
    (t) =>
      t.lexeme.toLowerCase().includes(filter.toLowerCase()) ||
      t.type.toLowerCase().includes(filter.toLowerCase())
  );

  const getBadgeColor = (type) => {
    if (KEYWORD_TYPES.has(type)) return 'bg-blue-500/20 text-blue-400 border-blue-500/30';
    if (TYPE_TOKENS.has(type)) return 'bg-teal-500/20 text-teal-400 border-teal-500/30';
    if (type.endsWith('_LITERAL')) return 'bg-amber-500/20 text-amber-400 border-amber-500/30';
    if (type === 'TOKEN_IDENTIFIER') return 'bg-purple-500/20 text-purple-400 border-purple-500/30';
    if (type === 'TOKEN_INCLUDE' || type === 'TOKEN_DEFINE' || type === 'TOKEN_HASH')
      return 'bg-fuchsia-500/20 text-fuchsia-300 border-fuchsia-500/30';
    if (type === 'TOKEN_EOF') return 'bg-gray-600/30 text-gray-300 border-gray-500/40';
    if (type === 'TOKEN_ERROR') return 'bg-red-500/20 text-red-400 border-red-500/30';
    // operators & punctuation
    return 'bg-yellow-500/20 text-yellow-300 border-yellow-500/30';
  };

  return (
    <div className="flex flex-col h-full bg-[#1e1e1e] p-4 text-xs font-mono">
      <div className="flex items-center justify-between mb-4">
        <div className="flex items-center space-x-2">
          <span className="font-bold text-gray-200">Phase 01: Lexical Analysis</span>
          <span className="bg-[#2d2d2d] px-2 py-0.5 rounded-full text-blue-400 text-[11px]">
            {tokens.length} tokens
          </span>
        </div>
        <div className="relative">
          <Search aria-hidden="true" className="w-3.5 h-3.5 absolute left-2.5 top-2 text-gray-500" />
          <label className="sr-only" htmlFor="token-filter">Filter tokens</label>
          <input
            id="token-filter"
            type="text"
            placeholder="Search tokens..."
            value={filter}
            onChange={(e) => setFilter(e.target.value)}
            className="bg-[#252526] border border-[#3c3c3c] rounded pl-8 pr-3 py-1 text-xs text-gray-200 focus:outline-none focus:border-blue-500"
          />
        </div>
      </div>

      {filteredTokens.length === 0 ? (
        <div className="flex-1 flex items-center justify-center text-gray-600 text-[11px]">
          {tokens.length === 0 ? 'Compile a program to see its token stream.' : 'No tokens match the filter.'}
        </div>
      ) : (
        <div className="flex-1 overflow-auto border border-[#333333] rounded-lg">
          <table className="w-full text-left border-collapse">
            <thead className="sticky top-0">
              <tr className="bg-[#252526] text-gray-400 border-b border-[#333333]">
                <th scope="col" className="p-2 border-r border-[#333333]">Index</th>
                <th scope="col" className="p-2 border-r border-[#333333]">Token Type</th>
                <th scope="col" className="p-2 border-r border-[#333333]">Lexeme</th>
                <th scope="col" className="p-2 border-r border-[#333333]">Line</th>
                <th scope="col" className="p-2">Column</th>
              </tr>
            </thead>
            <tbody>
              {filteredTokens.map((t, idx) => (
                <tr key={idx} className="border-b border-[#2a2a2a] hover:bg-[#252526] transition">
                  <td className="p-2 text-gray-500 border-r border-[#2a2a2a]">{idx + 1}</td>
                  <td className="p-2 border-r border-[#2a2a2a]">
                    <span className={`px-2 py-0.5 rounded border text-[11px] font-medium ${getBadgeColor(t.type)}`}>
                      {t.type}
                    </span>
                  </td>
                  <td className="p-2 font-bold text-gray-200 border-r border-[#2a2a2a]">{t.lexeme}</td>
                  <td className="p-2 text-gray-400 border-r border-[#2a2a2a]">{t.line}</td>
                  <td className="p-2 text-gray-400">{t.column}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}