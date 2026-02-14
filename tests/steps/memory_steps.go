package steps

import (
	"github.com/cucumber/godog"
)

func initMemorySteps(ctx *godog.ScenarioContext) {
	ctx.Step(`^node (\d+) writes to a shared page$`, nodeWritesSharedPage)
	ctx.Step(`^node (\d+) reads the same page$`, nodeReadsSharedPage)
	ctx.Step(`^node (\d+) should see the updated data$`, nodeShouldSeeUpdatedData)
	ctx.Step(`^pages are allocated on a NUMA system$`, pagesAllocatedNUMA)
	ctx.Step(`^pages should be allocated on the local NUMA node$`, pagesOnLocalNUMA)
	ctx.Step(`^pages are allocated up to the memory limit$`, pagesUpToLimit)
	ctx.Step(`^further allocations should be rejected cleanly$`, furtherAllocsRejected)
	ctx.Step(`^a sequential access pattern is detected$`, sequentialAccessPattern)
	ctx.Step(`^the prefetch window should increase$`, prefetchWindowIncrease)
	ctx.Step(`^a random access pattern is detected$`, randomAccessPattern)
	ctx.Step(`^prefetching should be disabled$`, prefetchDisabled)
	ctx.Step(`^KSM is enabled on the host$`, ksmEnabled)
	ctx.Step(`^UCK pages should not be merged by KSM$`, uckPagesNotMerged)
}

func nodeWritesSharedPage(node int) error     { return godog.ErrPending }
func nodeReadsSharedPage(node int) error      { return godog.ErrPending }
func nodeShouldSeeUpdatedData(node int) error { return godog.ErrPending }
func pagesAllocatedNUMA() error               { return godog.ErrPending }
func pagesOnLocalNUMA() error                 { return godog.ErrPending }
func pagesUpToLimit() error                   { return godog.ErrPending }
func furtherAllocsRejected() error            { return godog.ErrPending }
func sequentialAccessPattern() error          { return godog.ErrPending }
func prefetchWindowIncrease() error           { return godog.ErrPending }
func randomAccessPattern() error              { return godog.ErrPending }
func prefetchDisabled() error                 { return godog.ErrPending }
func ksmEnabled() error                       { return godog.ErrPending }
func uckPagesNotMerged() error                { return godog.ErrPending }
