package cli

import (
	"encoding/xml"
	"io"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/baseline"
)

type junitTestSuites struct {
	XMLName xml.Name     `xml:"testsuites"`
	Suites  []junitSuite `xml:"testsuite"`
}

type junitSuite struct {
	Name     string      `xml:"name,attr"`
	Tests    int         `xml:"tests,attr"`
	Failures int         `xml:"failures,attr"`
	Skipped  int         `xml:"skipped,attr"`
	Cases    []junitCase `xml:"testcase"`
}

type junitCase struct {
	Name      string        `xml:"name,attr"`
	ClassName string        `xml:"classname,attr"`
	Failure   *junitFailure `xml:"failure,omitempty"`
	Skipped   *junitSkip    `xml:"skipped,omitempty"`
}

type junitFailure struct {
	Message string `xml:"message,attr"`
	Body    string `xml:",chardata"`
}

type junitSkip struct {
	Message string `xml:"message,attr"`
}

func writeVerifyJUnit(w io.Writer, b *baseline.Baseline, changes []baseline.Change, summary baseline.Summary, showOK bool) error {
	_ = showOK
	return writeJUnit(w, "verify "+b.BaselineID, changes, summary)
}

func writeDiffJUnit(w io.Writer, oldB, newB *baseline.Baseline, changes []baseline.Change, summary baseline.Summary, showOK bool) error {
	_ = showOK
	return writeJUnit(w, "diff "+oldB.BaselineID+" -> "+newB.BaselineID, changes, summary)
}

func writeJUnit(w io.Writer, name string, changes []baseline.Change, summary baseline.Summary) error {
	cases := make([]junitCase, 0, len(changes))
	for _, c := range changes {
		tc := junitCase{
			Name:      c.Kind + " " + c.Schema + "." + c.Name,
			ClassName: c.Kind,
		}
		switch c.Status {
		case "ok":
		case "skipped":
			tc.Skipped = &junitSkip{Message: c.Reason}
		default:
			body := c.Status
			if c.BaselineChecksum != "" || c.CurrentChecksum != "" {
				body += " expected=" + display(c.BaselineChecksum) + " actual=" + display(c.CurrentChecksum)
			}
			if c.Reason != "" {
				body += " reason=" + c.Reason
			}
			tc.Failure = &junitFailure{Message: c.Status, Body: body}
		}
		cases = append(cases, tc)
	}
	doc := junitTestSuites{Suites: []junitSuite{{
		Name:     name,
		Tests:    summary.Total(),
		Failures: summary.Changed + summary.SchemaChanged + summary.New + summary.Missing,
		Skipped:  summary.Skipped,
		Cases:    cases,
	}}}
	if _, err := io.WriteString(w, xml.Header); err != nil {
		return err
	}
	enc := xml.NewEncoder(w)
	enc.Indent("", "  ")
	if err := enc.Encode(doc); err != nil {
		return err
	}
	_, err := io.WriteString(w, "\n")
	return err
}
