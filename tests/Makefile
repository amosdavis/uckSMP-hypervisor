.PHONY: all test clean deps

GODOG := go test -v -run "^$$" -godog.format=pretty

all: deps test

deps:
	go mod tidy

test:
	$(GODOG) ./...

test-security:
	$(GODOG) -godog.tags=@security ./...

test-concurrency:
	$(GODOG) -godog.tags=@concurrency ./...

test-reliability:
	$(GODOG) -godog.tags=@reliability ./...

test-migration:
	$(GODOG) -godog.tags=@migration ./...

test-networking:
	$(GODOG) -godog.tags=@networking ./...

test-scheduling:
	$(GODOG) -godog.tags=@scheduling ./...

test-operational:
	$(GODOG) -godog.tags=@operational ./...

test-architecture:
	$(GODOG) -godog.tags=@architecture ./...

clean:
	go clean -testcache
