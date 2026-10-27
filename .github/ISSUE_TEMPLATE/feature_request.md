---
name: Feature request
about: Suggest an improvement or new checksum capability
title: "[FEATURE]"
labels: enhancement
assignees: ''
type: Feature

---

### What problem would this solve?

<!-- Describe the use case, not only the proposed solution. -->

### What would you like to happen?

<!-- New behavior, function, argument, format, or performance improvement. -->

Example of the desired usage:

```sql
SELECT pg_...;
```

### Alternatives considered

<!-- What can be done today? Why is that not enough? -->

### Area of change

- [ ] new checksum level
- [ ] physical
- [ ] logical
- [ ] tables / indexes / databases
- [ ] partitioned objects
- [ ] TOAST
- [ ] parallel execution
- [ ] cross-database portability
- [ ] documentation / tests

### Compatibility

- [ ] this changes logical checksum values
- [ ] this changes the SQL API
- [ ] this is only a performance optimization; values stay the same
- [ ] this needs a new GUC or function argument
- [ ] this needs a migration or baseline recalculation

### Additional context

<!-- Examples, links, PostgreSQL version constraints. -->

### Checklist

- [ ] I searched for existing feature requests
- [ ] I described the problem, not only the solution
- [ ] I understand that changing the logical checksum format may be a breaking change
