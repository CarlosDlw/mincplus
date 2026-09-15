import type {SidebarsConfig} from '@docusaurus/plugin-content-docs';

// This runs in Node.js - don't use client-side code here (browser APIs, JSX...).

// The sidebar is written out rather than generated from the folder, for the same
// reason the command line is a table: the order is a decision. A reader arriving
// at the language reference should meet the types before the statements, and the
// statements before the preprocessor -- and that order should not change because
// a file was renamed.
const sidebars: SidebarsConfig = {
  languageSidebar: [
    'intro',
    {
      type: 'category',
      label: 'Getting started',
      collapsed: false,
      items: ['getting-started/install', 'getting-started/hello-world', 'getting-started/tour'],
    },
    {
      type: 'category',
      label: 'The language',
      collapsed: false,
      items: [
        'language/source',
        'language/types',
        'language/variables',
        'language/expressions',
        'language/statements',
        'language/functions',
        'language/pointers',
        'language/never',
        'language/memory-model',
        'language/preprocessor',
      ],
    },
    {
      type: 'category',
      label: 'Reference',
      collapsed: false,
      items: ['reference/grammar', 'reference/operators'],
    },
    {
      type: 'category',
      label: 'Tools',
      collapsed: false,
      items: ['tools/cli', 'tools/diagnostics', 'tools/architecture'],
    },
  ],
};

export default sidebars;
