OWNER(g:util)
SUBSCRIBER(g:util-subscribers)

PY2TEST()

SIZE(LARGE)

TAG(
    ya:force_sandbox 
    sb:intel_e5_2660v1
    ya:fat
)

TEST_SRCS(main.py)

DEPENDS(util/memory/benchmark/pool)

END()
