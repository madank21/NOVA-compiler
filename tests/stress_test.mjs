// Build and execute the full GNU C stress corpus with warnings promoted to
// errors. This validates the corpus itself; NOVA intentionally supports only a
// documented subset of the extensions exercised by this program.

import { execFileSync, spawnSync } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

if (spawnSync('gcc', ['--version'], { stdio: 'ignore' }).status !== 0) {
  console.log('stress: SKIPPED — gcc is not available.');
  process.exit(0);
}

const __dirname = dirname(fileURLToPath(import.meta.url));
const source = join(__dirname, 'corpus', 'stress_test.c');
const dir = mkdtempSync(join(tmpdir(), 'nova-stress-'));
const binary = join(dir, process.platform === 'win32' ? 'stress_test.exe' : 'stress_test');

try {
  execFileSync('gcc', [
    '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', source, '-lm', '-o', binary
  ], { stdio: 'pipe' });

  // The corpus intentionally writes debug/error-path demonstrations to stderr;
  // only stdout is needed for completion assertions.
  const run = spawnSync(binary, [], { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 });
  if (run.status !== 0) {
    throw new Error(`stress executable exited ${run.status}: ${run.stderr || ''}`);
  }
  const output = run.stdout || '';
  const suites = (output.match(/^=== Test \d+:/gm) || []).length;
  if (suites !== 12 || !output.includes('ALL TESTS COMPLETED')) {
    throw new Error(`incomplete stress output (suites=${suites})`);
  }
  console.log(`stress: PASS — clean -Werror build, ${suites} suites, exit 0`);
} finally {
  rmSync(dir, { recursive: true, force: true });
}