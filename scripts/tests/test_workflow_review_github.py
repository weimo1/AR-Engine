"""Untrusted PRs cannot select repositories, refs or trigger automatic paid work."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
import workflow_review_github as remote


class RemoteReviewTests(unittest.TestCase):
    def test_candidate_cannot_select_its_own_exclusions_or_review_rule(self):
        policy = {"version": "pinned", "excluded_review_context": {"third_party/**": "external"}}
        rule = {"exclude": ["third_party/**"]}
        remote.validate_policy(policy, policy, rule, rule)
        weakened = copy.deepcopy(policy)
        weakened["excluded_review_context"]["scripts/**"] = "hide code"
        with self.assertRaisesRegex(ValueError, "trusted-policy"):
            remote.validate_policy(policy, weakened, rule, rule)
        with self.assertRaisesRegex(ValueError, "trusted-policy"):
            remote.validate_policy(policy, policy, rule, {"exclude": ["**/*"]})

    def test_only_owner_same_repository_pr_with_exact_commits_is_admitted(self):
        event = {"repository": {"full_name": "weimo1/AR-Engine", "default_branch": "main",
                                "owner": {"login": "weimo1"}},
                 "pull_request": {"user": {"login": "weimo1"}, "head": {
                     "sha": "a" * 40, "repo": {"full_name": "weimo1/AR-Engine"}}, "base": {
                     "sha": "b" * 40, "ref": "main", "repo": {"full_name": "weimo1/AR-Engine"}}}}
        self.assertEqual(remote.validate_event(event, "weimo1/AR-Engine"), event["pull_request"])
        for mutate in (lambda x: x["pull_request"]["head"]["repo"].update(full_name="attacker/fork"),
                       lambda x: x["pull_request"]["user"].update(login="other"),
                       lambda x: x["pull_request"]["base"].update(ref="untrusted"),
                       lambda x: x["pull_request"]["head"].update(sha="HEAD; run-code")):
            changed = copy.deepcopy(event)
            mutate(changed)
            with self.assertRaises(ValueError):
                remote.validate_event(changed, "weimo1/AR-Engine")


if __name__ == "__main__":
    unittest.main()
