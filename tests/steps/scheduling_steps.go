package steps

import (
	"github.com/cucumber/godog"
)

func initSchedulingSteps(ctx *godog.ScenarioContext) {
	ctx.Step(`^heterogeneous nodes with (\d+)-CPU and (\d+)-CPU$`, heterogeneousNodes)
	ctx.Step(`^the (\d+)-CPU node should get approximately (\d+)x more forks$`, moreForks)
	ctx.Step(`^a node has low CPU but high memory pressure$`, lowCPUHighMem)
	ctx.Step(`^the load balancer should avoid that node$`, loadBalancerAvoids)
	ctx.Step(`^high contention operations run$`, highContentionOps)
	ctx.Step(`^no soft lockup warnings should appear in dmesg$`, noSoftLockup)
}

func heterogeneousNodes(cpuA, cpuB int) error { return godog.ErrPending }
func moreForks(cpuNode, multiplier int) error { return godog.ErrPending }
func lowCPUHighMem() error                    { return godog.ErrPending }
func loadBalancerAvoids() error               { return godog.ErrPending }
func highContentionOps() error                { return godog.ErrPending }
func noSoftLockup() error                     { return godog.ErrPending }
