from fastapi import APIRouter, Depends, HTTPException

from . import db
from .schemas import Rule, RuleIn
from .security import require_api_key

router = APIRouter(prefix="/api/rules")


@router.get("", response_model=list[Rule])
def list_rules() -> list[dict]:
    return db.list_rules()


@router.post("", response_model=Rule, status_code=201, dependencies=[Depends(require_api_key)])
def create_rule(rule: RuleIn) -> dict:
    return db.create_rule(rule.model_dump())


@router.put("/{rule_id}", response_model=Rule, dependencies=[Depends(require_api_key)])
def update_rule(rule_id: int, rule: RuleIn) -> dict:
    row = db.update_rule(rule_id, rule.model_dump())
    if row is None:
        raise HTTPException(404, "rule not found")
    return row


@router.delete("/{rule_id}", status_code=204, dependencies=[Depends(require_api_key)])
def delete_rule(rule_id: int) -> None:
    if not db.delete_rule(rule_id):
        raise HTTPException(404, "rule not found")
