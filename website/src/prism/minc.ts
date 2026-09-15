// Custom Prism grammar for minc+.
//
// This gives the docs proper syntax highlighting: keywords, types, operators,
// strings, comments, and the bottom type `!`. It is deliberately minimal —
// the docs are the language, not its parser, and a wrong highlight is worse
// than no highlight.

// The `Prism` type here is the class from prismjs, re-exported by
// prism-react-renderer. We accept it as `any` because the v2 types are
// structural and the language registration is a plain property assignment.
// eslint-disable-next-line @typescript-eslint/no-explicit-any
export default function defineMinC(Prism: any) {
  if (Prism.languages.minc) return; // already registered

  Prism.languages.minc = {
    // Line and block comments
    comment: [
      { pattern: /\/\/.*/, greedy: true },
      { pattern: /\/\*[\s\S]*?\*\//, greedy: true },
    ],

    // String literals
    string: {
      pattern: /"(?:[^"\\]|\\.)*"/,
      greedy: true,
    },

    // Character literals
    char: {
      pattern: /'(?:[^'\\]|\\.)'/,
      greedy: true,
    },

    // Preprocessor directives (lines starting with #)
    'preprocessor': {
      pattern: /^\s*#\s*\w+/m,
      alias: 'builtin',
    },

    // Keywords
    keyword: /\b(?:fn|let|const|return|if|else|while|for|break|continue|extern)\b/,

    // Types — primitive names and the bottom type
    'class-name': /\b(?:i8|i16|i32|i64|i128|isize|u8|u16|u32|u64|u128|usize|f32|f64|f80|bool|char|str|void|!)\b/,

    // Named constants
    builtin: /\b(?:true|false|null)\b/,

    // Number literals: hex, binary, octal, decimal, float
    number: /\b(?:0x[\da-fA-F_]+|0b[01_]+|0o[0-7_]+|\d[\d_]*\.?[\d_]*(?:e[+-]?[\d_]+)?)\b/,

    // Operators
    operator: /[+\-*/%=!<>&|^~?:]+|\.\.\.?/,

    // Punctuation
    punctuation: /[{}[\]();,.]/,

    // Function names (called)
    function: /\b[a-z_]\w*(?=\s*\()/,
  };
}
