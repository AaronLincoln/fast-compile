#pragma once 
#include <fc/thread/future.hpp>
#include <fc/any.hpp>
#include <functional>
#include <boost/config.hpp>
#include <boost/preprocessor/repeat.hpp>
#include <boost/preprocessor/repetition/enum_binary_params.hpp>
#include <boost/preprocessor/repetition/enum_params.hpp>
#include <boost/preprocessor/repetition/enum_trailing_params.hpp>
#include <boost/preprocessor/facilities/empty.hpp>

namespace fc {
  struct identity_member { 
       template<typename R, typename C, typename P, typename... Args>
       static std::function<R(Args...)> functor( P&& p, R (C::*mem_func)(Args...) ) {
         return std::function<R(Args...)>([=](Args... args){ return (p->*mem_func)(args...); });
       }
       template<typename R, typename C, typename P, typename... Args>
       static std::function<R(Args...)> functor( P&& p, R (C::*mem_func)(Args...)const ) {
         return std::function<R(Args...)>([=](Args... args){ return (p->*mem_func)(args...); });
       }
  };
 
   
  template< typename Interface, typename Transform  >
  struct vtable  : public std::enable_shared_from_this<vtable<Interface,Transform>> 
  {
     private:
        vtable();
  };
  

  template<typename OtherType>
  struct vtable_copy_visitor {
      typedef OtherType other_type;

      vtable_copy_visitor( OtherType& s):_source( s ){}
  
      template<typename R, typename MemberPtr, typename... Args>
      void operator()( const char* name, std::function<R(Args...)>& memb, MemberPtr m )const {
        OtherType* src = &_source;
        memb = [src,m]( Args... args ){ 
           wdump( (uint64_t(src) ) );
           return (src->*m)(args...); 
        };
      }
      OtherType& _source;
  };

  template<typename Interface, typename Transform = identity_member >
  class api {
    public:
      typedef vtable<Interface,Transform> vtable_type;

      api()
      :_vtable( std::make_shared<vtable_type>() )
      {}

      /** T is anything with pointer semantics */
      template<typename T >
      api( const T& p )
      :_vtable( std::make_shared<vtable_type>() )
      {
         _data = std::make_shared<fc::any>(p);
         T& ptr = boost::any_cast<T&>(*_data);
         auto& pointed_at = *ptr;
         typedef typename std::remove_reference<decltype(pointed_at)>::type source_vtable_type;
         _vtable->template visit_other( vtable_copy_visitor<source_vtable_type>(pointed_at) );
      }

      api( const api& cpy )
      :_vtable(cpy._vtable),_data(cpy._data)
      {
      }

      friend bool operator == ( const api& a, const api& b ) { return a._data == b._data && a._vtable == b._vtable;    }
      friend bool operator != ( const api& a, const api& b ) { return !(a._data == b._data && a._vtable == b._vtable); }

      vtable_type& operator*()const  { wdump((uint64_t(this))); assert(_vtable); FC_ASSERT( _vtable ); return *_vtable; }
      vtable_type* operator->()const {  
         assert(_vtable); 
         FC_ASSERT( _vtable ); 
         return _vtable.get();
      }
      void test();

    protected:
      std::shared_ptr<vtable_type>    _vtable;
      std::shared_ptr<fc::any>        _data;
  };


}

#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/stringize.hpp>

#define FC_API_VTABLE_DEFINE_MEMBER( r, data, elem ) \
      decltype(Transform::functor( (data*)nullptr, &data::elem)) elem; 
#define FC_API_VTABLE_DEFINE_VISIT_OTHER( r, data, elem ) \
        { typedef typename Visitor::other_type OtherType; \
        v( BOOST_PP_STRINGIZE(elem), elem, &OtherType::elem ); }
#define FC_API_VTABLE_DEFINE_VISIT( r, data, elem ) \
        v( BOOST_PP_STRINGIZE(elem), elem ); 

#define FC_API( CLASS, METHODS ) \
namespace fc { \
  template<typename Transform> \
  struct vtable<CLASS,Transform> : public std::enable_shared_from_this<vtable<CLASS,Transform>> { \
      BOOST_PP_SEQ_FOR_EACH( FC_API_VTABLE_DEFINE_MEMBER, CLASS, METHODS ) \
      template<typename Visitor> \
      void visit_other( Visitor&& v ){ \
        BOOST_PP_SEQ_FOR_EACH( FC_API_VTABLE_DEFINE_VISIT_OTHER, CLASS, METHODS ) \
      } \
      template<typename Visitor> \
      void visit( Visitor&& v ){ \
        BOOST_PP_SEQ_FOR_EACH( FC_API_VTABLE_DEFINE_VISIT, CLASS, METHODS ) \
      } \
  }; \
 }  
//#undef FC_API_VTABLE_DEFINE_MEMBER
//#undef FC_API_VTABLE_DEFINE_VISIT
