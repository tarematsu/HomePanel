# D1 query insights

Pull-request diagnostics collect the top D1 SQL statements for the preceding 24 hours, ranked independently by rows read and rows written. The JSON artifacts are retained for regression comparison without changing Worker runtime behavior.

The report is intended for before-and-after verification of each optimization against the latest main branch.
