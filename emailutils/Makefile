GO := go
GOFLAGS ?= -mod=readonly -ldflags "-s -w -extldflags '-static -O2'"

build:
	CGO_ENABLED=0 $(GO) build -a -trimpath -tags netgo $(GOFLAGS) -o bin/ ./cmd/...

distclean:
	@rm -fr bin

clean:
	@$(GO) clean

gofmt:
	@mkdir -p output
	@rm -f output/lint.log

	gofmt -d -s . 2>&1 | tee output/lint.log

	@[ ! -s output/lint.log ]

	@rm -fr output

tidy:
	@$(GO) mod tidy -v
