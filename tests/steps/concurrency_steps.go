package steps

import (
	"github.com/cucumber/godog"
)

func initConcurrencySteps(ctx *godog.ScenarioContext) {
	ctx.Step(`^multiple threads fault on the same page simultaneously$`, concurrentPageFault)
	ctx.Step(`^exactly one network fetch should occur$`, exactlyOneNetworkFetch)
	ctx.Step(`^all threads should see correct data$`, allThreadsSeeCorrectData)
	ctx.Step(`^concurrent operations run under lockdep$`, concurrentOpsUnderLockdep)
	ctx.Step(`^no lockdep warnings should be generated$`, noLockdepWarnings)
	ctx.Step(`^concurrent read and write faults occur on the same page$`, concurrentReadWriteFaults)
	ctx.Step(`^all final page states should be valid$`, allFinalPageStatesValid)
	ctx.Step(`^a process is migrated while pages are being faulted$`, migrateDuringFault)
	ctx.Step(`^no memory corruption should occur$`, noMemoryCorruption)
	ctx.Step(`^a futex wake is sent to a partitioned node$`, futexWakePartitioned)
	ctx.Step(`^the wake should timeout and notify the application$`, wakeTimeoutNotify)
	ctx.Step(`^a KASAN-enabled build runs stress tests$`, kasanStressTest)
	ctx.Step(`^no KASAN violations should be reported$`, noKASANViolations)
}

func concurrentPageFault() error      { return godog.ErrPending }
func exactlyOneNetworkFetch() error   { return godog.ErrPending }
func allThreadsSeeCorrectData() error { return godog.ErrPending }
func concurrentOpsUnderLockdep() error { return godog.ErrPending }
func noLockdepWarnings() error        { return godog.ErrPending }
func concurrentReadWriteFaults() error { return godog.ErrPending }
func allFinalPageStatesValid() error  { return godog.ErrPending }
func migrateDuringFault() error       { return godog.ErrPending }
func noMemoryCorruption() error       { return godog.ErrPending }
func futexWakePartitioned() error     { return godog.ErrPending }
func wakeTimeoutNotify() error        { return godog.ErrPending }
func kasanStressTest() error          { return godog.ErrPending }
func noKASANViolations() error        { return godog.ErrPending }
