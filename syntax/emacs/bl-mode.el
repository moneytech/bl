;;; bl-mode-el -- Major mode for editing BL files

;; Author: Martin Dorazil
;; Created: 25 Sep 2018
;; Keywords: BL major-mode

;;; Code:
(require 'js)

(defvar bl-mode-hook nil)
(defvar bl-mode-map
  (let ((bl-mode-map (make-keymap)))
    (define-key bl-mode-map "\C-j" 'newline-and-indent)
    bl-mode-map)
  "Keymap for BL major mode")

(add-to-list 'auto-mode-alist '("\\.bl\\'" . bl-mode))

(defconst bl-keywords
  '("if" "loop" "break" "continue" "else" "sizeof" "typeof" "alignof" "defer" "typeinfo" "typekind"
    "struct" "enum" "fn" "return" "cast" "auto" "unreachable" "switch" "default")) 

(defconst bl-types
  '("s8" "s16" "s32" "s64" "u8" "u16" "u32" "u64" "f32" "f64" "bool" "usize" "void" "string"
    "type"))

(defconst bl-constants
  '("true" "false" "null"))

(defconst bl-number-rx
  (rx (and
       symbol-start
       (or (and (+ digit) (opt (and (any "eE") (opt (any "-+")) (+ digit))))
           (and "0" (any "xX") (+ hex-digit)))
       (opt (and (any "_" "A-Z" "a-z") (* (any "_" "A-Z" "a-z" "0-9" "."))))
       symbol-end)))

(defun bl-wrap-word-rx (s)
  (concat "\\<" s "\\>"))

(defun bl-keywords-rx (keywords)
  "build keyword regexp"
  (bl-wrap-word-rx (regexp-opt keywords t)))

(defconst bl-font-lock-defaults
  `(
    ;; Keywords
    (,(bl-keywords-rx bl-keywords) 1 font-lock-keyword-face)

    ;; Types 
    (,(bl-keywords-rx bl-types) 1 font-lock-type-face)
    ("\\(\\w+\\)\\(.*\\)\\(\\:*enum\\)" 1 font-lock-function-name-face)
    ("\\(\\w+\\)\\(.*\\)\\(\\:*struct\\)" 1 font-lock-function-name-face)

    ;; Functions
    ("\\(\\w+\\)\\( *(\\)" 1 font-lock-function-name-face)
    ("\\(\\w+\\)\\(.*\\)\\(\\:*fn\\)" 1 font-lock-function-name-face)

    ;; Variables 
    ("\\(\\w+ \\)\\(.*\\)\\(\\:=\\)" 1 font-lock-variable-name-face)

    ;; Hash directives
    ("#\\w+" . font-lock-preprocessor-face)

    ;; Constants
    (,(bl-keywords-rx bl-constants) 1 font-lock-constant-face)
    ("\\(\\w+\\)\\(.*\\)\\(\\:\\)" 1 font-lock-constant-face)

    ;; Chars 
    ("\\\\'.*\\\\'" . font-lock-string-face)

    ;; Strings
    ("\\\".*\\\"" . font-lock-string-face)

    ;; Numbers
    (,(bl-wrap-word-rx bl-number-rx) . font-lock-constant-face)
    ))


(defvar bl-font-lock-keywords bl-font-lock-defaults
  "Default highlighting expressions for BL mode.")

(defvar bl-mode-syntax-table
  (let ((bl-mode-syntax-table (make-syntax-table)))
    
                                        ; This is added so entity names with underscores can be more easily parsed
    (modify-syntax-entry ?_ "w" bl-mode-syntax-table)
    
                                        ; Comment styles are same as C++
    (modify-syntax-entry ?/ ". 124b" bl-mode-syntax-table)
    (modify-syntax-entry ?* ". 23" bl-mode-syntax-table)
    (modify-syntax-entry ?\n "> b" bl-mode-syntax-table)
    bl-mode-syntax-table)
  "Syntax table for bl-mode")

(defun bl-mode ()
  (interactive)
  (setq-local indent-line-function 'js-indent-line)
  (setq-local indent-tabs-mode nil)
  (use-local-map bl-mode-map)

  (set-syntax-table bl-mode-syntax-table)
  ;; Set up font-lock
  (set (make-local-variable 'font-lock-defaults) '(bl-font-lock-keywords))
  (setq major-mode 'bl-mode)
  (setq mode-name "BL")
  (run-hooks 'bl-mode-hook))

(provide 'bl-mode)

;;; bl-mode.el ends here
