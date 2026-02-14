package steps

import (
	"github.com/cucumber/godog"
)

func initArchitectureSteps(ctx *godog.ScenarioContext) {
	ctx.Step(`^the split architecture is deployed$`, splitArchDeployed)
	ctx.Step(`^all existing functionality should work through the split$`, allFuncWorksThroughSplit)
	ctx.Step(`^the x86_64 HAL build is used$`, x86HALBuild)
	ctx.Step(`^register capture and restore should work$`, regCaptureRestoreWorks)
	ctx.Step(`^only core and heartbeat modules are loaded$`, onlyCoreAndHeartbeat)
	ctx.Step(`^reduced functionality should work correctly$`, reducedFuncWorks)
}

func splitArchDeployed() error       { return godog.ErrPending }
func allFuncWorksThroughSplit() error { return godog.ErrPending }
func x86HALBuild() error             { return godog.ErrPending }
func regCaptureRestoreWorks() error  { return godog.ErrPending }
func onlyCoreAndHeartbeat() error    { return godog.ErrPending }
func reducedFuncWorks() error        { return godog.ErrPending }
