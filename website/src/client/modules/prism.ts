// Client module: registers the minc+ Prism grammar so code blocks with
// ` ```minc ` or ` ```minc+ ` get proper syntax highlighting.
//
// This file is loaded on every page via `clientModules` in docusaurus.config.ts.
// The guard (`if Prism.languages.minc`) is defensive: in hot-reload or SSR, the
// module may be evaluated more than once.

import {Prism} from 'prism-react-renderer';
import defineMinC from '../../prism/minc';

defineMinC(Prism);
