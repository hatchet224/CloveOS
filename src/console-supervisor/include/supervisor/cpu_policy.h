#ifndef SUPERVISOR_CPU_POLICY_H
#define SUPERVISOR_CPU_POLICY_H
#include <stdbool.h>
struct sc_cpu_policy { char root[512], governor[64], speed[64]; bool changed; };
bool sc_cpu_policy_prepare(struct sc_cpu_policy *policy, const char *root);
bool sc_cpu_policy_restore(struct sc_cpu_policy *policy);
bool sc_cpu_policy_idle(const char *root);
#endif
