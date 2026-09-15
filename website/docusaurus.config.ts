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

  // Change this to the deployed origin; `onBrokenLinks: 'throw'` below makes a
  // link to a page that does not exist a build failure, not a warning.
  url: 'https://mincplus.dev',
  baseUrl: '/',

  onBrokenLinks: 'throw',
  markdown: {
    hooks: {
      onBrokenMarkdownLinks: 'throw',
    },
  },

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      {
        docs: {
          routeBasePath: '/',
          sidebarPath: './sidebars.ts',
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
      ],
      copyright: `Copyright © ${new Date().getFullYear()} minc+ contributors. Licensed under the MIT License.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
