// MathJax settings for pymdownx.arithmatex (generic mode): \( \) inline, \[ \] and $$ display.
window.MathJax = {
  tex: {inlineMath: [["\\(", "\\)"]], displayMath: [["\\[", "\\]"]], processEscapes: true, processEnvironments: true},
  options: {ignoreHtmlClass: ".*|", processHtmlClass: "arithmatex"},
};
document$.subscribe(() => { MathJax.typesetPromise(); });
