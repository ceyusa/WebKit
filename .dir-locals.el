;; Per-directory local variables for GNU Emacs 23 and later.

;; Syntax: ((MODE (VAR . VAL) ...) ...)
;; MODE is a symbol like `c-mode', or `nil' for all modes.
((c-mode
  (indent-tabs-mode . nil)
  (c-basic-offset . 4))
 (c++-mode
  (indent-tabs-mode . nil)
  (c-basic-offset . 4)
  (mode . clang-format+))
 (java-mode
  (indent-tabs-mode . nil)
  (c-basic-offset . 4))
 (ruby-mode
  (ruby-indent-level . 4))
 (change-log-mode
  (indent-tabs-mode . nil))
 (nil
  (fill-column . 100)
  (ccls-executable . "/home/vjaquez/.local/bin/webkit-ccls")
  (ccls-initialization-options . (:compilationDatabaseDirectory "/app/webkit/WebKitBuild/Release"
                                  :cache (:directory "/app/webkit/.ccls-cache")))
  (compile-command . "build-webkit --gtk --debug")
  (lsp-enable-file-watchers . nil)))
