package steps

import (
	"github.com/cucumber/godog"
)

func initNetworkingSteps(ctx *godog.ScenarioContext) {
	ctx.Step(`^all network operations work through the userspace proxy$`, opsWorkThroughProxy)
	ctx.Step(`^(\d+) nodes communicate simultaneously$`, nodesCommunicateSimultaneously)
	ctx.Step(`^no file descriptor exhaustion should occur$`, noFDExhaustion)
	ctx.Step(`^the MTU is reduced to (\d+)$`, mtuReduced)
	ctx.Step(`^page transfers should complete correctly$`, pageTransfersCorrect)
	ctx.Step(`^RDMA hardware is available$`, rdmaAvailable)
	ctx.Step(`^page fetch latency should be at least 10x better than TCP$`, rdmaLatencyBetter)
}

func opsWorkThroughProxy() error                 { return godog.ErrPending }
func nodesCommunicateSimultaneously(n int) error { return godog.ErrPending }
func noFDExhaustion() error                      { return godog.ErrPending }
func mtuReduced(mtu int) error                   { return godog.ErrPending }
func pageTransfersCorrect() error                { return godog.ErrPending }
func rdmaAvailable() error                       { return godog.ErrPending }
func rdmaLatencyBetter() error                   { return godog.ErrPending }
