import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

// This runs in Node.js - don't use client-side code here (browser APIs, JSX...).

// The documentation is the *language*, and it is the whole site: `routeBasePath`
// is `/`, so the introduction is the home page and there is no separate landing
// page to drift out of date. The compiler's internals -- why a stage is shaped
// the way it is, what the alternatives were -- are not here. They live in
// `docs/architecture.md` and `docs/architectures/` in the repository, next to the
// code they describe, because they are read while reading the code.
const config: Config = {
  title: 'minc+',
  tagline: 'A systems programming language with explicit types, explicit memory, and no surprises',
  favicon: 'img/favicon.ico',

  // Future flags, see https://docusaurus.io/docs/api/docusaurus-config#future
  future: {
    v4: true, // Improve compatibility with the upcoming Docusaurus v4
  },

  // The site is a GitHub Pages project site, so the origin is the account and
  // the repository is the base path. `onBrokenLinks: 'throw'` below makes a link
  // to a page that does not exist a build failure, not a warning.
  url: 'https://carlosdlw.github.io',
  baseUrl: '/mincplus/',

  onBrokenLinks: 'throw',
  markdown: {
    hooks: {
      onBrokenMarkdownLinks: 'throw',
    },
    mermaid: true,
  },

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  // Client modules run on every page load. The Prism registration adds
  // minc+ syntax highlighting to every ` ```minc ` code block on the site.
  clientModules: [
    './src/client/modules/prism.ts',
  ],

  presets: [
    [
      'classic',
      {
        docs: {
          routeBasePath: '/',
          sidebarPath: './sidebars.ts',
          // "Edit this page" on every doc page, linking to the source file on
          // GitHub so a reader can propose a fix without leaving the page.
          editUrl: 'https://github.com/CarlosDlw/mincplus/edit/main/website/',
        },
        // No blog: an announcement that goes stale is worse than no
        // announcement, and the release notes belong with the release.
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  // Local offline search. No external service, no API key.
  // The index is built at build time and served as a static JSON blob.
  plugins: [
    [
      '@easyops-cn/docusaurus-search-local',
      {
        indexDocs: true,
        indexBlog: false,
        indexPages: false,
        language: 'en',
      },
    ],
  ],

  // Mermaid diagrams: architecture, pipeline, data flow. Disabled by default
  // (themeClassNames), user toggled via the navbar button.
  themes: ['@docusaurus/theme-mermaid'],

  themeConfig: {
    colorMode: {
      respectPrefersColorScheme: true,
    },
    navbar: {
      title: 'minc+',
      logo: {
        alt: 'minc+',
        src: 'img/logo.svg',
      },
      items: [
        {
          type: 'docSidebar',
          sidebarId: 'languageSidebar',
          position: 'left',
          label: 'Documentation',
        },
        // The GitHub link on the right side of the navbar.
        {
          href: 'https://github.com/CarlosDlw/mincplus',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Language',
          items: [
            {label: 'Introduction', to: '/'},
            {label: 'Types', to: '/language/types'},
            {label: 'Grammar', to: '/reference/grammar'},
          ],
        },
        {
          title: 'Tools',
          items: [
            {label: 'Command line', to: '/tools/cli'},
            {label: 'Diagnostics', to: '/tools/diagnostics'},
          ],
        },
        {
          title: 'Community',
          items: [
            {label: 'GitHub', href: 'https://github.com/CarlosDlw/mincplus'},
            {label: 'Discussions', href: 'https://github.com/CarlosDlw/mincplus/discussions'},
          ],
        },
      ],
      copyright: `Copyright © ${new Date().getFullYear()} minc+ contributors. Licensed under the MIT License.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      // Custom language for minc+ syntax highlighting.
      additionalLanguages: [],
      magicComments: [
        {
          className: 'code-block-highlight-line',
          line: 'highlight-next-line',
        },
      ],
    },

    // Mermaid configuration: dark theme matches the site's dark mode.
    mermaid: {
      theme: {light: 'neutral', dark: 'dark'},
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
