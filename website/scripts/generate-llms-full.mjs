// Generates /llms-full.txt: all documentation pages concatenated into one file
// for LLM consumption. Runs after `docusaurus build` and writes into
// `build/llms-full.txt` so it ends up at the site root.
//
// The file is large by design: an agent can fetch the entire documentation in
// one HTTP request instead of crawling page by page. Each page is separated by
// a clear delimiter so a reader can split it if needed.

import {readdirSync, readFileSync, writeFileSync} from 'node:fs';
import {join, relative} from 'node:path';

const docsDir = join(process.cwd(), 'docs');
const outFile = join(process.cwd(), 'build', 'llms-full.txt');

// Collect all .md files using recursive readdir (stable since Node 18.17).
const files = readdirSync(docsDir, {recursive: true})
  .filter((f) => typeof f === 'string' && f.endsWith('.md'))
  .map((f) => join(docsDir, f))
  .sort();

let output = '# minc+ — full documentation\n\n';
output += '> All pages of the language reference, concatenated for LLM consumption.\n';
output += '> Each page starts with a heading and a separator.\n\n';

for (const file of files) {
  const rel = relative(docsDir, file);
  const content = readFileSync(file, 'utf-8');

  output += `---\n## ${rel}\n\n${content.trim()}\n\n`;
}

writeFileSync(outFile, output);
console.log(`llms-full.txt: ${files.length} pages, ${output.length} bytes`);
