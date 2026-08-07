/** @type {import('tailwindcss').Config} */
export default {
  content: [
    "./index.html",
    "./src/**/*.{js,ts,jsx,tsx}",
  ],
  theme: {
    extend: {
      colors: {
        vscode: {
          bg: '#1e1e1e',
          sidebar: '#252526',
          activeTab: '#1e1e1e',
          inactiveTab: '#2d2d2d',
          border: '#333333',
          lineNum: '#858585',
          activeLine: '#2c2c2d',
          keyword: '#569CD6',
          type: '#4EC9B0',
          string: '#CE9178',
          number: '#B5CEA8',
          comment: '#6A9955',
          function: '#DCDCAA',
          variable: '#9CDCFE',
          operator: '#D4D4D4'
        }
      }
    },
  },
  plugins: [],
}
