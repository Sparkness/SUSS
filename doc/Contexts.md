# Contexts

Contexts are collections of values which make up the state for which an action 
will be evaluated. You might have an action "Attack Target", but *which* target,
and using *which* attack ability? 

A context specifies those missing details and an action can be evaluated multiple
times with different contexts in a single brain update.

Contexts are generated by [Queries](Actions.md#queries).

The main elements of the Context are:

* Controlled Actor (the AI agent, mandatory)
* Target (a target actor, optional)
* Location (a world location, optional)

You can also add Named Values of many types, although in Blueprints
you won't see those in "Break Suss Context", you'll need to use utility functions
"Get Suss Context Value As ..."

In C++ you can even associate structs with named values in your contexts, although
it takes a little more work to expose those to Blueprints. For an example of this,
see Get Perception Info From Context.

# See Also

* [Home](../README.md)
* [Main classes](doc/MainClasses.md)
* [Brain Config](BrainConfig.md)
* [Brain Update Tick](BrainUpdate.md)
