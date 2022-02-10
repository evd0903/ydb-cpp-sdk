# Generated by devtools/yamaker (pypi).

PY2_LIBRARY() 

OWNER(blinkov g:python-contrib)

VERSION(2.5.2)

LICENSE(BSD-3-Clause)

NO_LINT()

NO_CHECK_IMPORTS(
    pygments.sphinxext
)

PY_SRCS(
    TOP_LEVEL
    pygments/__init__.py
    pygments/__main__.py
    pygments/cmdline.py
    pygments/console.py
    pygments/filter.py
    pygments/filters/__init__.py
    pygments/formatter.py
    pygments/formatters/__init__.py
    pygments/formatters/_mapping.py
    pygments/formatters/bbcode.py
    pygments/formatters/html.py
    pygments/formatters/img.py
    pygments/formatters/irc.py
    pygments/formatters/latex.py
    pygments/formatters/other.py
    pygments/formatters/rtf.py
    pygments/formatters/svg.py
    pygments/formatters/terminal.py
    pygments/formatters/terminal256.py
    pygments/lexer.py
    pygments/lexers/__init__.py
    pygments/lexers/_asy_builtins.py
    pygments/lexers/_cl_builtins.py
    pygments/lexers/_cocoa_builtins.py
    pygments/lexers/_csound_builtins.py
    pygments/lexers/_lasso_builtins.py
    pygments/lexers/_lua_builtins.py
    pygments/lexers/_mapping.py
    pygments/lexers/_mql_builtins.py
    pygments/lexers/_openedge_builtins.py
    pygments/lexers/_php_builtins.py
    pygments/lexers/_postgres_builtins.py
    pygments/lexers/_scilab_builtins.py
    pygments/lexers/_sourcemod_builtins.py
    pygments/lexers/_stan_builtins.py
    pygments/lexers/_stata_builtins.py
    pygments/lexers/_tsql_builtins.py
    pygments/lexers/_vbscript_builtins.py
    pygments/lexers/_vim_builtins.py
    pygments/lexers/actionscript.py
    pygments/lexers/agile.py
    pygments/lexers/algebra.py
    pygments/lexers/ambient.py
    pygments/lexers/ampl.py
    pygments/lexers/apl.py
    pygments/lexers/archetype.py
    pygments/lexers/asm.py
    pygments/lexers/automation.py
    pygments/lexers/basic.py
    pygments/lexers/bibtex.py
    pygments/lexers/boa.py
    pygments/lexers/business.py
    pygments/lexers/c_cpp.py
    pygments/lexers/c_like.py
    pygments/lexers/capnproto.py
    pygments/lexers/chapel.py
    pygments/lexers/clean.py
    pygments/lexers/compiled.py
    pygments/lexers/configs.py
    pygments/lexers/console.py
    pygments/lexers/crystal.py
    pygments/lexers/csound.py
    pygments/lexers/css.py
    pygments/lexers/d.py
    pygments/lexers/dalvik.py
    pygments/lexers/data.py
    pygments/lexers/diff.py
    pygments/lexers/dotnet.py
    pygments/lexers/dsls.py
    pygments/lexers/dylan.py
    pygments/lexers/ecl.py
    pygments/lexers/eiffel.py
    pygments/lexers/elm.py
    pygments/lexers/email.py
    pygments/lexers/erlang.py
    pygments/lexers/esoteric.py
    pygments/lexers/ezhil.py
    pygments/lexers/factor.py
    pygments/lexers/fantom.py
    pygments/lexers/felix.py
    pygments/lexers/floscript.py
    pygments/lexers/forth.py
    pygments/lexers/fortran.py
    pygments/lexers/foxpro.py
    pygments/lexers/freefem.py
    pygments/lexers/functional.py
    pygments/lexers/go.py
    pygments/lexers/grammar_notation.py
    pygments/lexers/graph.py
    pygments/lexers/graphics.py
    pygments/lexers/haskell.py
    pygments/lexers/haxe.py
    pygments/lexers/hdl.py
    pygments/lexers/hexdump.py
    pygments/lexers/html.py
    pygments/lexers/idl.py
    pygments/lexers/igor.py
    pygments/lexers/inferno.py
    pygments/lexers/installers.py
    pygments/lexers/int_fiction.py
    pygments/lexers/iolang.py
    pygments/lexers/j.py
    pygments/lexers/javascript.py
    pygments/lexers/julia.py
    pygments/lexers/jvm.py
    pygments/lexers/lisp.py
    pygments/lexers/make.py
    pygments/lexers/markup.py
    pygments/lexers/math.py
    pygments/lexers/matlab.py
    pygments/lexers/mime.py
    pygments/lexers/ml.py
    pygments/lexers/modeling.py
    pygments/lexers/modula2.py
    pygments/lexers/monte.py
    pygments/lexers/ncl.py
    pygments/lexers/nimrod.py
    pygments/lexers/nit.py
    pygments/lexers/nix.py
    pygments/lexers/oberon.py
    pygments/lexers/objective.py
    pygments/lexers/ooc.py
    pygments/lexers/other.py
    pygments/lexers/parasail.py
    pygments/lexers/parsers.py
    pygments/lexers/pascal.py
    pygments/lexers/pawn.py
    pygments/lexers/perl.py
    pygments/lexers/php.py
    pygments/lexers/pony.py
    pygments/lexers/praat.py
    pygments/lexers/prolog.py
    pygments/lexers/python.py
    pygments/lexers/qvt.py
    pygments/lexers/r.py
    pygments/lexers/rdf.py
    pygments/lexers/rebol.py
    pygments/lexers/resource.py
    pygments/lexers/rnc.py
    pygments/lexers/roboconf.py
    pygments/lexers/robotframework.py
    pygments/lexers/ruby.py
    pygments/lexers/rust.py
    pygments/lexers/sas.py
    pygments/lexers/scdoc.py
    pygments/lexers/scripting.py
    pygments/lexers/sgf.py
    pygments/lexers/shell.py
    pygments/lexers/slash.py
    pygments/lexers/smalltalk.py
    pygments/lexers/smv.py
    pygments/lexers/snobol.py
    pygments/lexers/solidity.py
    pygments/lexers/special.py
    pygments/lexers/sql.py
    pygments/lexers/stata.py
    pygments/lexers/supercollider.py
    pygments/lexers/tcl.py
    pygments/lexers/templates.py
    pygments/lexers/teraterm.py
    pygments/lexers/testing.py
    pygments/lexers/text.py
    pygments/lexers/textedit.py
    pygments/lexers/textfmts.py
    pygments/lexers/theorem.py
    pygments/lexers/trafficscript.py
    pygments/lexers/typoscript.py
    pygments/lexers/unicon.py
    pygments/lexers/urbi.py
    pygments/lexers/varnish.py
    pygments/lexers/verification.py
    pygments/lexers/web.py
    pygments/lexers/webmisc.py
    pygments/lexers/whiley.py
    pygments/lexers/x10.py
    pygments/lexers/xorg.py
    pygments/lexers/zig.py
    pygments/modeline.py
    pygments/plugin.py
    pygments/regexopt.py
    pygments/scanner.py
    pygments/sphinxext.py
    pygments/style.py
    pygments/styles/__init__.py
    pygments/styles/abap.py
    pygments/styles/algol.py
    pygments/styles/algol_nu.py
    pygments/styles/arduino.py
    pygments/styles/autumn.py
    pygments/styles/borland.py
    pygments/styles/bw.py
    pygments/styles/colorful.py
    pygments/styles/default.py
    pygments/styles/emacs.py
    pygments/styles/friendly.py
    pygments/styles/fruity.py
    pygments/styles/igor.py
    pygments/styles/inkpot.py
    pygments/styles/lovelace.py
    pygments/styles/manni.py
    pygments/styles/monokai.py
    pygments/styles/murphy.py
    pygments/styles/native.py
    pygments/styles/paraiso_dark.py
    pygments/styles/paraiso_light.py
    pygments/styles/pastie.py
    pygments/styles/perldoc.py
    pygments/styles/rainbow_dash.py
    pygments/styles/rrt.py
    pygments/styles/sas.py
    pygments/styles/solarized.py
    pygments/styles/stata_dark.py
    pygments/styles/stata_light.py
    pygments/styles/tango.py
    pygments/styles/trac.py
    pygments/styles/vim.py
    pygments/styles/vs.py
    pygments/styles/xcode.py
    pygments/token.py
    pygments/unistring.py
    pygments/util.py
)

RESOURCE_FILES(
    PREFIX contrib/python/Pygments/py2/
    .dist-info/METADATA
    .dist-info/entry_points.txt
    .dist-info/top_level.txt
)

END()
